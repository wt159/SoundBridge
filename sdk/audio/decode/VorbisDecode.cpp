#include "VorbisDecode.h"
#include "LogWrapper.h"
#include <algorithm>
#include <cstring>

#define LOG_TAG "VorbisDecode"

namespace {
// 全是垃圾时的护栏：连续跳页到这个数就放弃，不在一个坏页链里无限空转
// （与 ffmpeg 侧的 32 包同义，规格 4.3）。
constexpr int kMaxConsecutiveHoles = 32;
} // namespace

VorbisDecode::VorbisDecode(AudioDecodeCallback *callback)
    : m_callback(callback)
    , m_decSpec()
    , m_abortFlag(nullptr)
    , m_data(nullptr)
    , m_size(0)
    , m_pos(0)
    , m_dataSourcePtr(nullptr)
    , m_vf()
    , m_vfOpened(false)
    , m_interleavedBuf()
    , m_consecutiveHoles(0)
    , m_holeCount(0)
    , m_streamingFallbackTried(false)
    , m_streamingMode(false)
    , m_producedAudio(false)
{
}

VorbisDecode::~VorbisDecode()
{
    if (m_vfOpened) {
        ov_clear(&m_vf);
        m_vfOpened = false;
    }
    clearDecodeSpec();
}

size_t VorbisDecode::readCallback(void *ptr, size_t size, size_t nmemb, void *datasource)
{
    if (ptr == nullptr || datasource == nullptr || size == 0 || nmemb == 0) {
        return 0;
    }

    VorbisDecode *self = static_cast<VorbisDecode *>(datasource);
    size_t toRead      = size * nmemb;
    if (self->m_dataSourcePtr != nullptr) {
        off64_t remaining = static_cast<off64_t>(self->m_size) - static_cast<off64_t>(self->m_pos);
        if (remaining <= 0) {
            return 0;
        }
        if (static_cast<off64_t>(toRead) > remaining) {
            toRead = static_cast<size_t>(remaining);
        }
        ssize_t got = self->m_dataSourcePtr->readAt(static_cast<off64_t>(self->m_pos), ptr, toRead);
        if (got <= 0) {
            return 0;
        }
        self->m_pos += static_cast<size_t>(got);
        return static_cast<size_t>(got) / size;
    }

    if (self->m_data == nullptr || self->m_pos >= self->m_size) {
        return 0;
    }

    size_t remainBytes = self->m_size - self->m_pos;
    size_t copyBytes   = std::min(toRead, remainBytes);
    if (copyBytes == 0) {
        return 0;
    }

    std::memcpy(ptr, self->m_data + self->m_pos, copyBytes);
    self->m_pos += copyBytes;
    return copyBytes / size;
}

int VorbisDecode::seekCallback(void *datasource, ogg_int64_t offset, int whence)
{
    if (datasource == nullptr) {
        return -1;
    }

    VorbisDecode *self = static_cast<VorbisDecode *>(datasource);
    ogg_int64_t base   = 0;
    switch (whence) {
    case SEEK_SET:
        base = 0;
        break;
    case SEEK_CUR:
        base = static_cast<ogg_int64_t>(self->m_pos);
        break;
    case SEEK_END:
        base = static_cast<ogg_int64_t>(self->m_size);
        break;
    default:
        return -1;
    }

    ogg_int64_t target = base + offset;
    if (target < 0 || target > static_cast<ogg_int64_t>(self->m_size)) {
        return -1;
    }

    self->m_pos = static_cast<size_t>(target);
    return 0;
}

int VorbisDecode::closeCallback(void *datasource)
{
    if (datasource == nullptr) {
        return -1;
    }
    return 0;
}

long VorbisDecode::tellCallback(void *datasource)
{
    if (datasource == nullptr) {
        return -1;
    }
    VorbisDecode *self = static_cast<VorbisDecode *>(datasource);
    return static_cast<long>(self->m_pos);
}

void VorbisDecode::clearDecodeSpec()
{
    if (m_decSpec.lineData != nullptr) {
        for (int ch = 0; ch < m_decSpec.spec.numChannel; ch++) {
            delete[] m_decSpec.lineData[ch];
            m_decSpec.lineData[ch] = nullptr;
        }
        delete[] m_decSpec.lineData;
        m_decSpec.lineData = nullptr;
    }
    if (m_decSpec.lineSize != nullptr) {
        delete[] m_decSpec.lineSize;
        m_decSpec.lineSize = nullptr;
    }
    m_decSpec.spec = AudioSpec();
}

bool VorbisDecode::ensureDecodeSpec(int channels, int sampleRate, uint64_t samples)
{
    if (channels <= 0 || sampleRate <= 0 || samples == 0) {
        return false;
    }

    if (m_decSpec.lineData == nullptr || m_decSpec.lineSize == nullptr
        || m_decSpec.spec.numChannel != channels) {
        clearDecodeSpec();
        m_decSpec.spec.numChannel     = channels;
        m_decSpec.spec.bitsPerSample  = 16;
        m_decSpec.spec.bytesPerSample = 2;
        m_decSpec.spec.format         = getAudioFormatByBitPreSample(m_decSpec.spec.bitsPerSample);
        m_decSpec.spec.sampleRate     = sampleRate;
        m_decSpec.lineData            = new uint8_t *[channels];
        m_decSpec.lineSize            = new int[channels];
        for (int ch = 0; ch < channels; ++ch) {
            m_decSpec.lineData[ch] = nullptr;
            m_decSpec.lineSize[ch] = 0;
        }
    } else {
        m_decSpec.spec.sampleRate = sampleRate;
    }

    if (m_decSpec.spec.samples != samples) {
        m_decSpec.spec.samples = samples;
        for (int ch = 0; ch < channels; ++ch) {
            delete[] m_decSpec.lineData[ch];
            m_decSpec.lineSize[ch]
                = static_cast<int>(samples * static_cast<uint64_t>(m_decSpec.spec.bytesPerSample));
            m_decSpec.lineData[ch] = new uint8_t[m_decSpec.lineSize[ch]];
        }
    }
    return true;
}

bool VorbisDecode::open(bool seekable)
{
    if (m_vfOpened) {
        ov_clear(&m_vf);
        m_vfOpened = false;
    }

    m_pos              = 0;
    m_consecutiveHoles = 0;
    m_producedAudio    = false;
    m_streamingMode    = !seekable;
    m_streamingFallbackTried = false;

    ov_callbacks callbacks;
    callbacks.read_func  = &VorbisDecode::readCallback;
    callbacks.seek_func  = seekable ? &VorbisDecode::seekCallback : nullptr;
    callbacks.close_func = &VorbisDecode::closeCallback;
    callbacks.tell_func  = seekable ? &VorbisDecode::tellCallback : nullptr;

    int ret = ov_open_callbacks(this, &m_vf, nullptr, 0, callbacks);
    if (ret != 0) {
        LOGE("ov_open_callbacks failed: %d (seekable=%d)", ret, seekable ? 1 : 0);
        return false;
    }
    m_vfOpened = true;

    vorbis_info *vi = ov_info(&m_vf, -1);
    if (vi == nullptr || vi->channels <= 0 || vi->rate <= 0) {
        LOGE("ov_info failed after open");
        ov_clear(&m_vf);
        m_vfOpened = false;
        return false;
    }

    LOGI("Bitstream is %d channel, %ldHz (seekable=%d)", vi->channels, vi->rate, seekable ? 1 : 0);
    return true;
}

bool VorbisDecode::reopenAsStreaming()
{
    // 只在数据源模式下有意义：内存模式（initVF）本来就是整块缓冲，没有预热路径。
    if (m_dataSourcePtr == nullptr) {
        return false;
    }
    return open(false);
}

bool VorbisDecode::initVF(const char *data, size_t size)
{
    if (data == nullptr || size == 0) {
        LOGE("initVF invalid input, data=%p size=%zu", data, size);
        return false;
    }

    m_data          = reinterpret_cast<const uint8_t *>(data);
    m_size          = size;
    m_dataSourcePtr = nullptr;
    return open(true);
}

bool VorbisDecode::initVFFromSource(DataSourceBase *src, off64_t size)
{
    if (src == nullptr || size <= 0) {
        return false;
    }

    m_dataSourcePtr = src;
    m_data          = nullptr;
    m_size          = static_cast<size_t>(size);
    return open(true);
}

int VorbisDecode::decodeOne()
{
    if (m_abortFlag != nullptr && m_abortFlag->load()) {
        return -1;
    }

    if (!m_vfOpened) {
        return -1;
    }

    constexpr int kReadBytes = 8192;
    if (m_interleavedBuf.size() != kReadBytes) {
        m_interleavedBuf.resize(kReadBytes);
    }

    int bitstream = 0;
    long bytes = ov_read(&m_vf, reinterpret_cast<char *>(m_interleavedBuf.data()), kReadBytes, 0, 2,
                         1, &bitstream);

    if (bytes == 0) {
        // 条目 13 的成因：可 seek 打开时 libvorbisfile 会走到 _open_seekable2() 的
        // ov_raw_seek() 预热。预热循环里 `_get_next_page() < 0`（首个音频页是空洞时
        // 就是这种情形）被当成「到文件尾」，它把 pcm_offset 置成 total 后直接 break，
        // 结果一个包都没进 vf->os，函数仍返回 0。于是 init 成功、ov_read 立刻 0(OV_EOF)，
        // 「0 字节且不报错」。这里退化成不可 seek 的流式打开：从第一页起逐页读，
        // 空洞按 OV_HOLE 跳过，代价是该文件不能再 ov_time_seek。
        if (!m_streamingFallbackTried && !m_streamingMode && !m_producedAudio) {
            m_streamingFallbackTried = true;
            LOGW("seekable open buffered no audio page (hole on the first audio page); "
                 "reopening as a non-seekable stream");
            if (reopenAsStreaming()) {
                return decodeOne();
            }
        }
        return 0;
    }

    if (bytes == OV_HOLE) {
        // `OV_HOLE(-3)` 是「这一页坏了，后面还有数据」，不是 `-1` 的整段失败
        // （规格 4.1：坏帧之后还有数据）。
        ++m_holeCount;
        if (++m_consecutiveHoles >= kMaxConsecutiveHoles) {
            LOGE("giving up after %d consecutive ogg holes", m_consecutiveHoles);
            return -1;
        }
        LOGW("ogg hole #%d: skipping the damaged page", m_holeCount);
        return kVorbisHoleSkipped;
    }

    if (bytes < 0) {
        LOGE("ov_read failed: %ld", bytes);
        return -1;
    }
    m_consecutiveHoles = 0;
    m_producedAudio    = true;

    vorbis_info *vi = ov_info(&m_vf, bitstream);
    if (vi == nullptr || vi->channels <= 0 || vi->rate <= 0) {
        LOGE("ov_info failed in decodeOne");
        return -1;
    }

    size_t frameBytes     = static_cast<size_t>(bytes);
    size_t channels       = static_cast<size_t>(vi->channels);
    size_t oneSampleBytes = channels * 2;
    if (oneSampleBytes == 0 || frameBytes % oneSampleBytes != 0) {
        LOGE("unexpected pcm bytes: %zu channels: %zu", frameBytes, channels);
        return -1;
    }

    uint64_t samples = frameBytes / oneSampleBytes;
    if (!ensureDecodeSpec(vi->channels, vi->rate, samples)) {
        LOGE("ensureDecodeSpec failed");
        return -1;
    }

    const int16_t *src = reinterpret_cast<const int16_t *>(m_interleavedBuf.data());
    for (uint64_t i = 0; i < samples; ++i) {
        for (int ch = 0; ch < vi->channels; ++ch) {
            int16_t value = src[i * static_cast<size_t>(vi->channels) + static_cast<size_t>(ch)];
            std::memcpy(m_decSpec.lineData[ch] + i * 2, &value, sizeof(value));
        }
    }

    if (m_callback != nullptr) {
        m_callback->onAudioDecodeCallback(m_decSpec);
    }
    return 1;
}

bool VorbisDecode::seekToMs(uint64_t targetMs)
{
    if (!m_vfOpened) {
        return false;
    }
    if (m_streamingMode && m_streamingFallbackTried) {
        // 退化打开的流没有 seek_func，ov_time_seek 必然 OV_ENOSEEK
        LOGE("seek unavailable on a stream reopened after a hole");
        return false;
    }

    double targetSec = static_cast<double>(targetMs) / 1000.0;
    int ret          = ov_time_seek(&m_vf, targetSec);
    if (ret != 0) {
        LOGE("ov_time_seek failed: %d, targetMs=%llu", ret,
             static_cast<unsigned long long>(targetMs));
        return false;
    }
    return true;
}

int VorbisDecode::decode(const char *data, ssize_t size)
{
    if (size <= 0) {
        return -1;
    }

    if (!initVF(data, static_cast<size_t>(size))) {
        return -1;
    }

    while (true) {
        int ret = decodeOne();
        if (ret == 0) {
            return 0;
        }
        if (ret < 0) {
            return -1;
        }
    }
}

void VorbisDecode::setAbortFlag(std::atomic<bool> *flag)
{
    m_abortFlag = flag;
}

int VorbisDecode::decode(AudioBufferPtr &inBuf)
{
    if (inBuf == nullptr) {
        return -1;
    }
    return decode(inBuf->data(), inBuf->size());
}
