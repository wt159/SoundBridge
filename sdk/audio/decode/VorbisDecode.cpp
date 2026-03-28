#include "VorbisDecode.h"
#include "LogWrapper.h"
#include <algorithm>
#include <cstring>

#define LOG_TAG "VorbisDecode"

VorbisDecode::VorbisDecode(AudioDecodeCallback *callback)
    : m_callback(callback)
    , m_decSpec()
    , m_abortFlag(nullptr)
    , m_data(nullptr)
    , m_size(0)
    , m_pos(0)
    , m_vf()
    , m_vfOpened(false)
    , m_interleavedBuf()
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
    if (self->m_data == nullptr || self->m_pos >= self->m_size) {
        return 0;
    }

    size_t requestBytes = size * nmemb;
    size_t remainBytes  = self->m_size - self->m_pos;
    size_t copyBytes    = std::min(requestBytes, remainBytes);
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

bool VorbisDecode::initVF(const char *data, size_t size)
{
    if (data == nullptr || size == 0) {
        LOGE("initVF invalid input, data=%p size=%zu", data, size);
        return false;
    }

    if (m_vfOpened) {
        ov_clear(&m_vf);
        m_vfOpened = false;
    }

    m_data = reinterpret_cast<const uint8_t *>(data);
    m_size = size;
    m_pos  = 0;

    ov_callbacks callbacks;
    callbacks.read_func  = &VorbisDecode::readCallback;
    callbacks.seek_func  = &VorbisDecode::seekCallback;
    callbacks.close_func = &VorbisDecode::closeCallback;
    callbacks.tell_func  = &VorbisDecode::tellCallback;

    int ret = ov_open_callbacks(this, &m_vf, nullptr, 0, callbacks);
    if (ret != 0) {
        LOGE("ov_open_callbacks failed: %d", ret);
        return false;
    }

    m_vfOpened = true;

    vorbis_info *vi = ov_info(&m_vf, -1);
    if (vi == nullptr) {
        LOGE("ov_info failed after open");
        ov_clear(&m_vf);
        m_vfOpened = false;
        return false;
    }

    LOGI("Bitstream is %d channel, %ldHz", vi->channels, vi->rate);
    return true;
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
        return 0;
    }

    if (bytes < 0) {
        LOGE("ov_read failed: %ld", bytes);
        return -1;
    }

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
