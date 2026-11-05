#include "FLACFrameReader.h"
#include <algorithm>
#include <cstring>

namespace {

// 帧头最长 16 字节：4 字节固定段 + 6 字节 UTF-8 编号 + 2 字节块长/采样率扩展 + CRC-8。
const size_t kMinHeaderBytes = 16;
const size_t kReadChunk      = 8 * 1024;

uint8_t crc8(const uint8_t *data, size_t size)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x07)
                               : static_cast<uint8_t>(crc << 1);
        }
    }
    return crc;
}

uint16_t crc16(const uint8_t *data, size_t size)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < size; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x8005)
                                 : static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

// FLAC 帧头（格式 9.1）：同步码 + 保留位与码表检查 + 帧头 CRC-8。
// needMore 表示「数据不够，补读之后重试」；否则就是确定不是帧头。
bool parseFrameHeader(const uint8_t *data, size_t available, bool atEndOfStream, bool &needMore,
                      size_t &headerBytes)
{
    needMore = false;
    if (available < kMinHeaderBytes) {
        needMore = !atEndOfStream;
        return false;
    }
    if (data[0] != 0xFF || (data[1] & 0xFE) != 0xF8) {
        return false; // 14 位同步码或保留位不对
    }
    const uint8_t blockSizeCode  = static_cast<uint8_t>(data[2] >> 4);
    const uint8_t sampleRateCode = static_cast<uint8_t>(data[2] & 0x0F);
    const uint8_t channelMode    = static_cast<uint8_t>(data[3] >> 4);
    const uint8_t sampleSizeCode = static_cast<uint8_t>((data[3] >> 1) & 0x07);
    if (blockSizeCode == 0 || sampleRateCode == 0x0F || sampleSizeCode == 3
        || sampleSizeCode == 7 || channelMode > 10) {
        return false; // 非法 / 保留编码
    }

    size_t cursor       = 4;
    size_t continuation = 0;
    const uint8_t first = data[cursor];
    if (first < 0x80) {
        continuation = 0;
    } else if ((first & 0xE0) == 0xC0) {
        continuation = 1;
    } else if ((first & 0xF0) == 0xE0) {
        continuation = 2;
    } else if ((first & 0xF8) == 0xF0) {
        continuation = 3;
    } else if ((first & 0xFC) == 0xF8) {
        continuation = 4;
    } else if ((first & 0xFE) == 0xFC) {
        continuation = 5;
    } else {
        return false; // UTF-8 编号首字节非法
    }
    cursor += 1 + continuation;
    if (blockSizeCode == 6) {
        cursor += 1;
    } else if (blockSizeCode == 7) {
        cursor += 2;
    }
    if (sampleRateCode == 12) {
        cursor += 1;
    } else if (sampleRateCode == 13 || sampleRateCode == 14) {
        cursor += 2;
    }

    if (cursor + 1 > available) {
        needMore = !atEndOfStream;
        return false;
    }
    if (crc8(data, cursor) != data[cursor]) {
        return false;
    }
    headerBytes = cursor + 1;
    return true;
}

uint16_t readBe16(const uint8_t *data)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

} // namespace

FLACFrameReader::FLACFrameReader(DataSourceBase *source, off64_t streamHeaderEnd, off64_t endOffset,
                                 size_t maxFrameBytes)
    : m_source(source)
    , m_streamHeaderEnd(streamHeaderEnd)
    , m_endOffset(endOffset)
    , m_nextOffset(0)
    , m_scanCap(0)
    , m_pendingBytes(0)
    , m_buf()
{
    size_t cap = std::max<size_t>(maxFrameBytes, 32 * 1024) * 2;
    if (cap > kMaxScanBytes) {
        cap = kMaxScanBytes;
    }
    if (cap < 64 * 1024) {
        cap = 64 * 1024;
    }
    m_scanCap = cap;
    if (m_streamHeaderEnd > m_endOffset) {
        m_streamHeaderEnd = m_endOffset;
    }
}

FLACFrameReader::~FLACFrameReader() { }

void FLACFrameReader::seekTo(off64_t offset)
{
    if (offset < 0) {
        offset = 0;
    }
    if (offset > m_endOffset) {
        offset = m_endOffset;
    }
    m_nextOffset = offset;
    m_buf.clear();
    m_pendingBytes = 0;
}

bool FLACFrameReader::nextPacket(AudioBuffer::AudioBufferPtr &packet)
{
    packet.reset();
    m_buf.clear();
    m_pendingBytes = 0;
    if (m_source == nullptr || m_nextOffset >= m_endOffset) {
        return false;
    }

    // 首包是流头（fLaC + 元数据块）：libFLAC 要先看到 STREAMINFO 才会做整段 MD5 校验。
    if (m_nextOffset < m_streamHeaderEnd) {
        const size_t length = static_cast<size_t>(m_streamHeaderEnd - m_nextOffset);
        ensureBytes(length);
        if (m_buf.empty()) {
            return false;
        }
        return emit(std::min(length, m_buf.size()), packet);
    }

    return produceFrame(packet);
}

bool FLACFrameReader::produceFrame(AudioBuffer::AudioBufferPtr &packet)
{
    // 1) 起点必须是帧头；认不出就向后找同步点，把这段垃圾当作一包交出去。
    size_t headerBytes = 0;
    size_t scanned     = 0;
    for (;;) {
        ensureBytes(kMinHeaderBytes);
        if (headerAt(0, headerBytes)) {
            break;
        }
        const size_t next = findHeader(1, scanned);
        if (next != kNotFound) {
            return emit(next, packet);
        }
        if (atEnd() || m_buf.size() >= m_scanCap) {
            return m_buf.empty() ? false : emit(m_buf.size(), packet);
        }
        appendChunk(0);
    }

    // 2) 帧尾：下一个通过帧头检查、并且帧尾 CRC-16 与整帧自洽的位置。
    size_t searchFrom = headerBytes;
    scanned           = searchFrom;
    for (;;) {
        const size_t candidate = findHeader(searchFrom, scanned);
        if (candidate == kNotFound) {
            if (atEnd() || m_buf.size() >= m_scanCap) {
                return emit(m_buf.size(), packet);
            }
            appendChunk(0);
            continue;
        }
        if (candidate >= 2 && candidate <= m_buf.size()
            && crc16(m_buf.data(), candidate - 2)
                == readBe16(m_buf.data() + candidate - 2)) {
            return emit(candidate, packet);
        }
        searchFrom = candidate + 1;
    }
}

bool FLACFrameReader::emit(size_t length, AudioBuffer::AudioBufferPtr &packet)
{
    if (length == 0 || length > m_buf.size()) {
        return false;
    }
    AudioBuffer::AudioBufferPtr out = std::make_shared<AudioBuffer>(length);
    std::memcpy(out->data(), m_buf.data(), length);

    m_pendingBytes = length;
    m_nextOffset += static_cast<off64_t>(length);
    m_buf.clear();
    // 交出之后包由调用方持有，提取器不再记一份（解码侧按 alloc/free 记账）。
    m_pendingBytes = 0;
    packet         = out;
    return true;
}

bool FLACFrameReader::appendChunk(size_t want)
{
    if (m_source == nullptr || atEnd()) {
        return false;
    }
    const size_t loaded = m_buf.size();
    const off64_t remain
        = m_endOffset - (m_nextOffset + static_cast<off64_t>(loaded));
    if (remain <= 0) {
        return false;
    }
    size_t size = std::max(kReadChunk, want > loaded ? want - loaded : 0);
    if (size > static_cast<size_t>(remain)) {
        size = static_cast<size_t>(remain);
    }
    if (loaded + size > m_scanCap) {
        if (loaded >= m_scanCap) {
            return false;
        }
        size = m_scanCap - loaded;
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

void FLACFrameReader::ensureBytes(size_t want)
{
    while (m_buf.size() < want && !atEnd() && m_buf.size() < m_scanCap) {
        if (!appendChunk(want)) {
            break;
        }
    }
}

bool FLACFrameReader::atEnd() const
{
    return m_nextOffset + static_cast<off64_t>(m_buf.size()) >= m_endOffset;
}

bool FLACFrameReader::headerAt(size_t offset, size_t &headerBytes) const
{
    if (offset >= m_buf.size()) {
        return false;
    }
    bool needMore = false;
    return parseFrameHeader(m_buf.data() + offset, m_buf.size() - offset, atEnd(), needMore,
                            headerBytes);
}

size_t FLACFrameReader::findHeader(size_t from, size_t &scanned)
{
    if (scanned < from) {
        scanned = from;
    }
    const size_t limit = m_buf.size() >= kMinHeaderBytes ? m_buf.size() - kMinHeaderBytes + 1 : 0;
    for (size_t pos = scanned; pos < limit; ++pos) {
        if (m_buf[pos] != 0xFF || (m_buf[pos + 1] & 0xFE) != 0xF8) {
            continue;
        }
        size_t headerBytes = 0;
        bool needMore      = false;
        if (parseFrameHeader(m_buf.data() + pos, m_buf.size() - pos, atEnd(), needMore,
                             headerBytes)) {
            scanned = pos + 1;
            return pos;
        }
        if (needMore) {
            // 补读之后从同一位置重试，别把半个帧头判死。
            scanned = pos;
            return kNotFound;
        }
    }
    scanned = limit;
    return kNotFound;
}
