#include "AACFrameReader.h"
#include <algorithm>
#include <cstring>

namespace {
const size_t kReadChunk = 8 * 1024;
} // namespace

bool AACFrameReader::parseHeader(const uint8_t *data, size_t available, size_t &frameLength)
{
    frameLength = 0;
    if (data == nullptr || available < kHeaderBytes) {
        return false;
    }
    if (data[0] != 0xFF || (data[1] & 0xF0) != 0xF0) {
        return false;
    }
    const size_t length = (static_cast<size_t>(data[3] & 0x03) << 11)
        | (static_cast<size_t>(data[4]) << 3) | ((static_cast<size_t>(data[5]) & 0xE0) >> 5);
    if (length < kHeaderBytes) {
        return false;
    }
    frameLength = length;
    return true;
}

size_t AACFrameReader::scanFrameCount(DataSourceBase *source, off64_t audioStart,
                                      off64_t audioEnd)
{
    if (source == nullptr || audioStart < 0 || audioEnd <= audioStart) {
        return 0;
    }
    size_t count = 0;
    off64_t pos  = audioStart;
    while (pos + static_cast<off64_t>(kHeaderBytes) <= audioEnd) {
        uint8_t header[kHeaderBytes];
        if (source->readAt(pos, header, sizeof(header))
            != static_cast<ssize_t>(sizeof(header))) {
            break;
        }
        size_t length = 0;
        if (!parseHeader(header, sizeof(header), length)) {
            break;
        }
        if (pos + static_cast<off64_t>(length) > audioEnd) {
            break;
        }
        ++count;
        pos += static_cast<off64_t>(length);
    }
    return count;
}

AACFrameReader::AACFrameReader(DataSourceBase *source, off64_t audioStart, off64_t audioEnd)
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

AACFrameReader::~AACFrameReader() { }

void AACFrameReader::seekTo(off64_t offset)
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

bool AACFrameReader::nextPacket(AudioBuffer::AudioBufferPtr &packet)
{
    packet.reset();
    m_buf.clear();
    if (m_source == nullptr || m_nextOffset >= m_audioEnd) {
        return false;
    }

    ensureBytes(kHeaderBytes);
    if (m_buf.size() < kHeaderBytes) {
        return false;
    }

    size_t frameLength = 0;
    if (parseHeader(m_buf.data(), m_buf.size(), frameLength)) {
        // 声明帧长越过文件尾：截断，不是坏帧。尾部残留不交出去。
        if (static_cast<off64_t>(frameLength) > m_audioEnd - m_nextOffset) {
            return false;
        }
        ensureBytes(frameLength);
        if (m_buf.size() < frameLength) {
            return false;
        }
        return emit(frameLength, packet);
    }

    // 起点不是 ADTS 头：向后找下一个同步点，把坏数据段当作一包交出去。
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
        scanned = before >= kHeaderBytes ? before - kHeaderBytes + 1 : 1;
    }
}

size_t AACFrameReader::findHeader(size_t from)
{
    if (m_buf.size() < kHeaderBytes) {
        return kNotFound;
    }
    const size_t limit = m_buf.size() - kHeaderBytes + 1;
    for (size_t pos = from; pos < limit; ++pos) {
        if (m_buf[pos] != 0xFF || (m_buf[pos + 1] & 0xF0) != 0xF0) {
            continue;
        }
        size_t length = 0;
        if (!parseHeader(m_buf.data() + pos, m_buf.size() - pos, length)) {
            continue;
        }
        // 帧长必须落在文件内，否则不是可用的同步点。
        if (m_nextOffset + static_cast<off64_t>(pos + length) > m_audioEnd) {
            continue;
        }
        return pos;
    }
    return kNotFound;
}

bool AACFrameReader::emit(size_t length, AudioBuffer::AudioBufferPtr &packet)
{
    if (length == 0 || length > m_buf.size()) {
        return false;
    }
    AudioBuffer::AudioBufferPtr out = std::make_shared<AudioBuffer>(length);
    std::memcpy(out->data(), m_buf.data(), length);

    m_nextOffset += static_cast<off64_t>(length);
    m_buf.clear();
    packet = out;
    return true;
}

bool AACFrameReader::appendChunk(size_t want)
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

void AACFrameReader::ensureBytes(size_t want)
{
    while (m_buf.size() < want && !atEnd() && m_buf.size() < kMaxScanBytes) {
        if (!appendChunk(want)) {
            break;
        }
    }
}

bool AACFrameReader::atEnd() const
{
    return m_nextOffset + static_cast<off64_t>(m_buf.size()) >= m_audioEnd;
}
