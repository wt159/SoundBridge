#include "OGGExtractor.h"
#include "ByteUtils.h"
#include "LogWrapper.h"
#include "type_name.hpp"
#include <cstring>
#include <vector>

#define LOG_TAG "OGGExtractor"

using namespace sdk_utils;
using namespace OGG;

namespace {

constexpr uint16_t kMaxCodecParameterNum = 15;

// 一页的 O(1) 视图：头 + 段表 + 负载位置。init 只读扫描不留页表，
// 所以这里按 offset 逐页读，读完就丢（ADR 0003「读过 ≠ 常驻」）。
struct PageView {
    uint8_t headerType      = 0;
    uint64_t granule        = 0;
    uint32_t serial         = 0;
    uint8_t numSegments     = 0;
    uint8_t segmentTable[PAGE_SEGMENT_NUM_MAX] = { 0 };
    off64_t payloadOffset   = 0;
    off64_t nextOffset      = 0;
};

// 读一页的头与段表，并算出负载范围。返回 false 表示：读不到完整页（截断 / 链断）。
// 不校验 CRC（与改造前一致）：页链完整性只影响「还能读多少」，不在这里判可解性。
bool readPage(DataSourceBase *source, off64_t fileSize, off64_t offset, PageView &page)
{
    const off64_t kHeaderBytes = 27;
    if (source == nullptr || offset < 0 || offset + kHeaderBytes > fileSize) {
        return false;
    }
    uint8_t header[kHeaderBytes];
    if (source->readAt(offset, header, sizeof(header))
        != static_cast<ssize_t>(sizeof(header))) {
        return false;
    }
    if (memcmp(header, PAGE_IDENTIFICATION, 4) != 0) {
        return false;
    }
    page.headerType  = header[5];
    page.granule     = U64LE_AT(header + 6);
    page.serial      = U32LE_AT(header + 14);
    page.numSegments = header[26];
    if (source->readAt(offset + kHeaderBytes, page.segmentTable, page.numSegments)
        != static_cast<ssize_t>(page.numSegments)) {
        return false;
    }
    size_t payloadBytes = 0;
    for (uint8_t i = 0; i < page.numSegments; ++i) {
        payloadBytes += page.segmentTable[i];
    }
    page.payloadOffset = offset + kHeaderBytes + page.numSegments;
    page.nextOffset    = page.payloadOffset + static_cast<off64_t>(payloadBytes);
    if (page.nextOffset > fileSize) {
        return false;
    }
    return true;
}

// codec 识别与改造前逐项一致；多出的只是长度检查（旧代码对短包 memcmp 越界读）。
AudioCodecID detectCodec(const uint8_t *data, size_t size, AudioSpec &spec)
{
    if (data == nullptr) {
        return AUDIO_CODEC_ID_NONE;
    }
    if (size >= 5 && memcmp(data, "\177fLaC", 5) == 0) {
        return AUDIO_CODEC_ID_FLAC;
    }
    if (size >= 8 && memcmp(data, "OpusHead", 8) == 0) {
        return AUDIO_CODEC_ID_OPUS;
    }
    if (size >= 7 && memcmp(data, "\x01vorbis", 7) == 0) {
        if (size >= 16) {
            spec.numChannel = static_cast<uint8_t>(data[11]);
            spec.sampleRate = U32LE_AT(data + 12);
        }
        return AUDIO_CODEC_ID_VORBIS;
    }
    if (size >= 8 && memcmp(data, "Speex   ", 8) == 0) {
        return AUDIO_CODEC_ID_SPEEX;
    }
    if (size >= 8 && memcmp(data, "CELT    ", 8) == 0) {
        return AUDIO_CODEC_ID_CELT;
    }
    if (size >= 8 && memcmp(data, "PCM     ", 8) == 0) {
        return AUDIO_CODEC_ID_NONE;
    }
    return AUDIO_CODEC_ID_NONE;
}

} // namespace

static bool readHeader(DataSourceBase *source, uint8_t *buf, size_t size)
{
    if (source == nullptr || buf == nullptr || size == 0) {
        return false;
    }
    return source->readAt(0, buf, size) >= static_cast<ssize_t>(size);
}

bool OGGExtractor::sniff(DataSourceBase *source)
{
    uint8_t buf[4] = { 0 };
    if (!readHeader(source, buf, sizeof(buf))) {
        return false;
    }
    return memcmp(buf, "OggS", 4) == 0;
}

OGGExtractor::OGGExtractor(DataSourceBase *source)
    : m_dataSource(source)
    , m_audioCodecID(AUDIO_CODEC_ID_NONE)
    , m_initCheck(NO_INIT)
    , m_validFormat(false)
    , m_spec()
    , m_dataSize(0)
{
    m_initCheck = init();
}

OGGExtractor::~OGGExtractor() { }

status_t OGGExtractor::init()
{
    off64_t fileSize = 0;
    if (m_dataSource == nullptr || m_dataSource->getSize(&fileSize) != OK || fileSize <= 0) {
        return NO_INIT;
    }
    m_dataSize = fileSize;

    // init 只读扫描（ADR 0003）：逐页走过与改造前相同的页链，只为拿
    //   1) 首个音频包的 codec 签名与 spec（前 15 个段之内）；
    //   2) 音频流末页的 granulePosition（= 累计 PCM 采样数，换算成标称时长）。
    // 页表与每页负载都不留：Vorbis / FLAC 的解码由 libvorbisfile / libFLAC
    // 在播放期经 DataSource 回调直接读，native random access 不受影响。
    bool firstPageSeen   = false;
    bool audioFound      = false;
    uint32_t audioSerial = 0;
    uint64_t lastGranule = 0;
    int segmentsExamined = 0;
    off64_t offset       = 0;

    while (offset < fileSize) {
        PageView page;
        if (!readPage(m_dataSource, fileSize, offset, page)) {
            if (!firstPageSeen) {
                // 首页都不是完整页：不是 ogg，或者头部结构自身不自洽（头坏）。
                return NO_INIT;
            }
            // 后续页断了 = 截断。按 ADR 0002 降级：能读多少算多少，不新增拒绝出口。
            LOGW("ogg page chain stops at %lld (file size %lld)", (long long)offset,
                 (long long)fileSize);
            break;
        }
        firstPageSeen = true;
        m_validFormat = true;

        if (!audioFound) {
            off64_t segmentOffset = page.payloadOffset;
            for (uint8_t i = 0; i < page.numSegments; ++i) {
                const size_t length = page.segmentTable[i];
                if (length > 0) {
                    uint8_t signature[16] = { 0 };
                    const size_t probe    = length < sizeof(signature) ? length : sizeof(signature);
                    if (m_dataSource->readAt(segmentOffset, signature, probe)
                        == static_cast<ssize_t>(probe)) {
                        const AudioCodecID codec = detectCodec(signature, probe, m_spec);
                        if (codec != AUDIO_CODEC_ID_NONE
                            || (length >= 8 && memcmp(signature, "PCM     ", 8) == 0)) {
                            m_audioCodecID = codec;
                            audioSerial    = page.serial;
                            audioFound     = true;
                            break;
                        }
                    }
                }
                segmentOffset += static_cast<off64_t>(length);
                if (++segmentsExamined > kMaxCodecParameterNum) {
                    break;
                }
            }
        }

        if (audioFound && page.serial == audioSerial && page.granule > 0) {
            lastGranule = page.granule;
        }
        offset = page.nextOffset;
    }

    if (!m_validFormat || !audioFound) {
        if (!audioFound) {
            LOGE("can not find audio pkt");
        }
        return NO_INIT;
    }

    // 时长归属（#15）：末页 granulePosition 是音频流累计的 PCM 采样数
    //（Vorbis / FLAC / Speex 语义一致）。不取的话 durationMs 恒为 0。
    if (lastGranule > 0 && m_spec.sampleRate > 0) {
        m_spec.durationMs = lastGranule * 1000ULL / static_cast<uint64_t>(m_spec.sampleRate);
        LOGD("durationMs from last granule: %llu samples -> %llu ms",
             (unsigned long long)lastGranule, (unsigned long long)m_spec.durationMs);
    }

    normalizeAudioSpec(m_spec);
    return OK;
}
