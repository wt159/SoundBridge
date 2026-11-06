#include "MP3FrameReader.h"
#include <algorithm>
#include <cstring>

namespace {

// 与旧 GetMPEGAudioFrameSize 的报错顺序保持一致：先同步码，再 version / layer，
// 再码率 / 采样率索引。码表也逐项照抄，避免 init 期与播放期对同一份素材给出不同边界。
const size_t kMinHeaderBytes = 4;
const size_t kReadChunk       = 8 * 1024;

uint32_t readBe32(const uint8_t *data)
{
    return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16)
        | (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
}

} // namespace

namespace mp3 {

FrameInfo parseFrameHeader(uint32_t header)
{
    FrameInfo info;
    if ((header & 0xffe00000) != 0xffe00000) {
        return info;
    }

    const unsigned version = (header >> 19) & 3;
    if (version == 0x01) {
        return info;
    }

    const unsigned layer = (header >> 17) & 3;
    if (layer == 0x00) {
        return info;
    }

    const unsigned bitrateIndex = (header >> 12) & 0x0f;
    if (bitrateIndex == 0 || bitrateIndex == 0x0f) {
        return info; // 不接受 free bitrate
    }

    const unsigned samplingRateIndex = (header >> 10) & 3;
    if (samplingRateIndex == 3) {
        return info;
    }

    static const int kSamplingRateV1[] = { 44100, 48000, 32000 };
    int sampleRate                     = kSamplingRateV1[samplingRateIndex];
    if (version == 2 /* V2 */) {
        sampleRate /= 2;
    } else if (version == 0 /* V2.5 */) {
        sampleRate /= 4;
    }

    const unsigned padding = (header >> 9) & 1;

    size_t frameSize  = 0;
    int bitrate       = 0;
    int samplesPerFrame = 1152;

    if (layer == 3) {
        // layer I
        static const int kBitrateV1[]
            = { 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448 };
        static const int kBitrateV2[]
            = { 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256 };

        bitrate = (version == 3 /* V1 */) ? kBitrateV1[bitrateIndex - 1]
                                          : kBitrateV2[bitrateIndex - 1];
        frameSize = (12000 * bitrate / sampleRate + padding) * 4;
        samplesPerFrame = 384;
    } else {
        // layer II or III
        static const int kBitrateV1L2[]
            = { 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384 };
        static const int kBitrateV1L3[]
            = { 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 };
        static const int kBitrateV2[]
            = { 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 };

        if (version == 3 /* V1 */) {
            bitrate = (layer == 2 /* L2 */) ? kBitrateV1L2[bitrateIndex - 1]
                                            : kBitrateV1L3[bitrateIndex - 1];
            samplesPerFrame = 1152;
        } else {
            bitrate         = kBitrateV2[bitrateIndex - 1];
            samplesPerFrame = (layer == 1 /* L3 */) ? 576 : 1152;
        }

        if (version == 3 /* V1 */) {
            frameSize = 144000 * bitrate / sampleRate + padding;
        } else {
            const size_t tmp = (layer == 1 /* L3 */) ? 72000 : 144000;
            frameSize        = tmp * bitrate / sampleRate + padding;
        }
    }

    if (frameSize == 0) {
        return info;
    }

    info.frameSize      = frameSize;
    info.sampleRate     = sampleRate;
    info.channels       = ((header >> 6) & 3) == 3 ? 1 : 2;
    info.bitrateKbps    = bitrate;
    info.samplesPerFrame = samplesPerFrame;
    info.valid          = true;
    return info;
}

} // namespace mp3

MP3FrameReader::MP3FrameReader(DataSourceBase *source, int64_t audioStart, int64_t audioEnd)
    : m_source(source)
    , m_audioStart(audioStart)
    , m_audioEnd(audioEnd)
    , m_nextOffset(audioStart)
    , m_buf()
{
    if (m_audioStart < 0) {
        m_audioStart = 0;
    }
    if (m_audioEnd < m_audioStart) {
        m_audioEnd = m_audioStart;
    }
    if (m_nextOffset < m_audioStart) {
        m_nextOffset = m_audioStart;
    }
}

MP3FrameReader::~MP3FrameReader() { }

void MP3FrameReader::seekTo(int64_t offset)
{
    if (offset < m_audioStart) {
        offset = m_audioStart;
    }
    if (offset > m_audioEnd) {
        offset = m_audioEnd;
    }
    m_nextOffset = offset;
    m_buf.clear();
}

bool MP3FrameReader::nextPacket(AudioBuffer::AudioBufferPtr &packet)
{
    packet.reset();
    m_buf.clear();
    if (m_source == nullptr || m_nextOffset >= m_audioEnd) {
        return false;
    }

    ensureBytes(kMinHeaderBytes);
    if (m_buf.size() < kMinHeaderBytes) {
        return false;
    }

    const mp3::FrameInfo info = mp3::parseFrameHeader(readBe32(m_buf.data()));
    if (info.valid) {
        // 声明帧长越过文件尾：这是截断，不是坏帧。尾部残留不交出去。
        if (static_cast<off64_t>(info.frameSize) > m_audioEnd - m_nextOffset) {
            return false;
        }
        ensureBytes(info.frameSize);
        if (m_buf.size() < info.frameSize) {
            return false;
        }
        return emit(info.frameSize, packet);
    }

    // 起点不是帧头：向后找下一个候选，把这段坏数据当作一包交出去，
    // 由解码器失败并记一次跳帧。找不到就停下。
    size_t scanned = 1;
    for (;;) {
        const size_t candidate = findHeader(scanned);
        if (candidate != kNotFound) {
            return emit(candidate, packet);
        }
        if (atEnd() || m_buf.size() >= kMaxScanBytes) {
            return false;
        }
        const size_t before = m_buf.size();
        appendChunk(0);
        if (m_buf.size() == before) {
            return false;
        }
        scanned = before >= 4 ? before - 3 : 1;
    }
}

bool MP3FrameReader::candidateAt(size_t pos) const
{
    if (pos + kMinHeaderBytes > m_buf.size()) {
        return false;
    }
    const uint32_t header     = readBe32(m_buf.data() + pos);
    const mp3::FrameInfo info = mp3::parseFrameHeader(header);
    if (!info.valid) {
        return false;
    }
    // 能读到后继帧头时要求掩码一致：同一码流的 version / layer / 采样率不变。
    // 读不到就只按当前头判断（帧可能正好在缓冲末尾）。
    const size_t next = pos + info.frameSize;
    if (next + kMinHeaderBytes <= m_buf.size()) {
        const mp3::FrameInfo successor = mp3::parseFrameHeader(readBe32(m_buf.data() + next));
        if (!successor.valid
            || (readBe32(m_buf.data() + next) & mp3::kFrameHeaderMask)
                != (header & mp3::kFrameHeaderMask)) {
            return false;
        }
    }
    return true;
}

size_t MP3FrameReader::findHeader(size_t from)
{
    if (m_buf.size() < kMinHeaderBytes) {
        return kNotFound;
    }
    const size_t limit = m_buf.size() - kMinHeaderBytes + 1;
    for (size_t pos = from; pos < limit; ++pos) {
        // 先做便宜的同步码过滤，再走完整帧头检查。
        if (m_buf[pos] != 0xFF || (m_buf[pos + 1] & 0xE0) != 0xE0) {
            continue;
        }
        if (candidateAt(pos)) {
            return pos;
        }
    }
    return kNotFound;
}

bool MP3FrameReader::emit(size_t length, AudioBuffer::AudioBufferPtr &packet)
{
    if (length == 0 || length > m_buf.size()) {
        return false;
    }
    AudioBuffer::AudioBufferPtr out = std::make_shared<AudioBuffer>(length);
    std::memcpy(out->data(), m_buf.data(), length);

    m_nextOffset += static_cast<off64_t>(length);
    m_buf.clear();
    // 交出之后包由调用方持有，提取器不再记一份（解码侧按 alloc/free 记账）。
    packet = out;
    return true;
}

bool MP3FrameReader::appendChunk(size_t want)
{
    if (m_source == nullptr || atEnd()) {
        return false;
    }
    const size_t loaded = m_buf.size();
    const off64_t remain = m_audioEnd - (m_nextOffset + static_cast<off64_t>(loaded));
    if (remain <= 0) {
        return false;
    }
    size_t size = std::max(kReadChunk, want > loaded ? want - loaded : 0);
    if (size > static_cast<size_t>(remain)) {
        size = static_cast<size_t>(remain);
    }
    if (loaded + size > kMaxScanBytes) {
        if (loaded >= kMaxScanBytes) {
            return false;
        }
        size = kMaxScanBytes - loaded;
    }

    m_buf.reserve(loaded + size);
    m_buf.resize(loaded + size);
    const ssize_t got = m_source->readAt(m_nextOffset + static_cast<off64_t>(loaded),
                                         m_buf.data() + loaded, size);
    if (got <= 0) {
        m_buf.resize(loaded);
        return false;
    }
    m_buf.resize(loaded + static_cast<size_t>(got));
    return true;
}

void MP3FrameReader::ensureBytes(size_t want)
{
    while (m_buf.size() < want && !atEnd() && m_buf.size() < kMaxScanBytes) {
        if (!appendChunk(want)) {
            break;
        }
    }
}

bool MP3FrameReader::atEnd() const
{
    return m_nextOffset + static_cast<off64_t>(m_buf.size()) >= m_audioEnd;
}
