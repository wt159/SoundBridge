#include "MP3Extractor.h"
#include "ByteUtils.h"
#include "ID3.h"
#include "LogWrapper.h"
#include <cstring>

#define LOG_TAG  "MP3Extractor"

#define __unused __attribute__((unused))

using namespace sdk_utils;
static bool readHeader(DataSourceBase *source, uint8_t *buf, size_t size)
{
    if (source == nullptr || buf == nullptr || size == 0) {
        return false;
    }
    return source->readAt(0, buf, size) >= static_cast<ssize_t>(size);
}

bool MP3Extractor::sniff(DataSourceBase *source)
{
    uint8_t buf[3] = { 0 };
    if (!readHeader(source, buf, sizeof(buf))) {
        return false;
    }
    if (memcmp(buf, "ID3", 3) == 0) {
        return true;
    }
    return (buf[0] == 0xFF && (buf[1] & 0xE0) == 0xE0);
}

// 帧头码表搬到 MP3FrameReader.cpp：init 期的 Resync 与播放期的包工厂共用
// 同一份解析（mp3::parseFrameHeader），「什么算合法帧头」不会出现第二套解释。
// 同一码流里 version / layer / sampleRate 必须一致，比对掩码仍是 mp3::kFrameHeaderMask。
using mp3::kFrameHeaderMask;
using mp3::FrameInfo;

static bool GetMPEGAudioFrameSize(uint32_t header, size_t *frame_size,
                                  int *out_sampling_rate = NULL, int *out_channels = NULL,
                                  int *out_bitrate = NULL, int *out_num_samples = NULL)
{
    const FrameInfo info = mp3::parseFrameHeader(header);
    *frame_size          = info.frameSize;
    if (out_sampling_rate) {
        *out_sampling_rate = info.sampleRate;
    }
    if (out_channels) {
        *out_channels = info.channels;
    }
    if (out_bitrate) {
        *out_bitrate = info.bitrateKbps;
    }
    if (out_num_samples) {
        *out_num_samples = info.samplesPerFrame;
    }
    return info.valid;
}

static bool Resync(DataSourceBase *source, uint32_t match_header, off64_t *inout_pos,
                   off64_t *post_id3_pos, uint32_t *out_header)
{
    if (post_id3_pos != NULL) {
        *post_id3_pos = 0;
    }

    if (*inout_pos == 0) {
        // Skip an optional ID3 header if syncing at the very beginning
        // of the datasource.

        for (;;) {
            uint8_t id3header[10];
            if (source->readAt(*inout_pos, id3header, sizeof(id3header))
                < (ssize_t)sizeof(id3header)) {
                // If we can't even read these 10 bytes, we might as well bail
                // out, even if there _were_ 10 bytes of valid mp3 audio data...
                return false;
            }

            if (memcmp("ID3", id3header, 3)) {
                break;
            }

            // Skip the ID3v2 header.

            size_t len = ((id3header[6] & 0x7f) << 21) | ((id3header[7] & 0x7f) << 14)
                | ((id3header[8] & 0x7f) << 7) | (id3header[9] & 0x7f);

            len += 10;

            // 声明长度必须先和文件剩余比一次（规格 10.#18）。声明超出文件时这个
            // tag 不可信：只跳过 10 字节头，把后面的字节当音频继续找帧同步，
            // 而不是按声明推进到文件之外——那样 Resync 必然失败、整首解出 0 字节。
            off64_t fileSize = 0;
            if (source->getSize(&fileSize) != OK || fileSize < *inout_pos) {
                fileSize = *inout_pos;
            }
            const off64_t declaredEnd = *inout_pos + static_cast<off64_t>(len);
            if (declaredEnd > fileSize) {
                LOGW("ID3 tag declares %llu bytes but only %lld remain; "
                     "treating it as a header-only tag",
                     (unsigned long long)len, (long long)(fileSize - *inout_pos));
                *inout_pos += 10;
                break;
            }
            *inout_pos = declaredEnd;

            LOGV("skipped ID3 tag, new starting offset is %lld (0x%016llx)", (long long)*inout_pos,
                 (long long)*inout_pos);
        }

        if (post_id3_pos != NULL) {
            *post_id3_pos = *inout_pos;
        }
    }

    off64_t pos = *inout_pos;
    bool valid  = false;

    const size_t kMaxReadBytes    = 1024;
    const size_t kMaxBytesChecked = 128 * 1024;
    uint8_t buf[kMaxReadBytes];
    ssize_t bytesToRead    = kMaxReadBytes;
    ssize_t totalBytesRead = 0;
    ssize_t remainingBytes = 0;
    bool reachEOS          = false;
    uint8_t *tmp           = buf;

    do {
        if (pos >= (off64_t)(*inout_pos + kMaxBytesChecked)) {
            // Don't scan forever.
            LOGV("giving up at offset %lld", (long long)pos);
            break;
        }

        if (remainingBytes < 4) {
            if (reachEOS) {
                break;
            } else {
                memcpy(buf, tmp, remainingBytes);
                bytesToRead = kMaxReadBytes - remainingBytes;

                /*
                 * The next read position should start from the end of
                 * the last buffer, and thus should include the remaining
                 * bytes in the buffer.
                 */
                totalBytesRead
                    = source->readAt(pos + remainingBytes, buf + remainingBytes, bytesToRead);
                if (totalBytesRead <= 0) {
                    break;
                }
                reachEOS        = (totalBytesRead != bytesToRead);
                totalBytesRead += remainingBytes;
                remainingBytes  = totalBytesRead;
                tmp             = buf;
                continue;
            }
        }

        uint32_t header = U32_AT(tmp);

        if (match_header != 0
            && (header & kFrameHeaderMask) != (match_header & kFrameHeaderMask)) {
            ++pos;
            ++tmp;
            --remainingBytes;
            continue;
        }

        size_t frame_size;
        int sample_rate, num_channels, bitrate;
        if (!GetMPEGAudioFrameSize(header, &frame_size, &sample_rate, &num_channels, &bitrate)) {
            ++pos;
            ++tmp;
            --remainingBytes;
            continue;
        }

        LOGV("found possible 1st frame at %lld (header = 0x%08x)", (long long)pos, header);

        // We found what looks like a valid frame,
        // now find its successors.

        off64_t test_pos = pos + frame_size;

        valid = true;
        for (int j = 0; j < 3; ++j) {
            uint8_t tmp[4];
            if (source->readAt(test_pos, tmp, 4) < 4) {
                valid = false;
                break;
            }

            uint32_t test_header = U32_AT(tmp);

            LOGV("subsequent header is %08x", test_header);

            if ((test_header & kFrameHeaderMask) != (header & kFrameHeaderMask)) {
                valid = false;
                break;
            }

            size_t test_frame_size;
            if (!GetMPEGAudioFrameSize(test_header, &test_frame_size)) {
                valid = false;
                break;
            }

            LOGV("found subsequent frame #%d at %lld", j + 2, (long long)test_pos);

            test_pos += test_frame_size;
        }

        if (valid) {
            *inout_pos = pos;

            if (out_header != NULL) {
                *out_header = header;
            }
        } else {
            LOGV("no dice, no valid sequence of frames found.");
        }

        ++pos;
        ++tmp;
        --remainingBytes;
    } while (!valid);

    return valid;
}

MP3Extractor::MP3Extractor(DataSourceBase *source)
    : m_initCheck(NO_INIT)
    , m_dataSource(source)
    , m_audioCodecID(AUDIO_CODEC_ID_NONE)
    , m_metaBuf(nullptr)
    , m_validFormat(false)
    , m_spec()
    , m_firstFramePos(-1)
    , m_fixedHeader(0)
    , m_dataSize(0)
    , m_audioEnd(0)
    , m_reader()
    , m_nextPacketIndex(0)
    , m_sequenceOffset(0)
{
    off64_t pos = 0;
    off64_t post_id3_pos;
    uint32_t header;
    bool success;

    success = Resync(m_dataSource, 0, &pos, &post_id3_pos, &header);
    if (!success) {
        // m_initCheck will remain NO_INIT
        return;
    }
    LOGI("pos: %lld, post_id3_pos: %lld, header: %x", pos, post_id3_pos, header);
    m_firstFramePos = pos;
    m_fixedHeader   = header;

    // XINGSeeker *seeker = XINGSeeker::CreateFromSource(m_dataSource, mFirstFramePos);
    // if (seeker == NULL) {
    //     mSeeker = VBRISeeker::CreateFromSource(m_dataSource, post_id3_pos);
    // } else {
    //     mSeeker = seeker;
    //     int encd = seeker->getEncoderDelay();
    //     int encp = seeker->getEncoderPadding();
    //     if (encd != 0 || encp != 0) {
    //         mMeta.setInt32(kKeyEncoderDelay, encd);
    //         mMeta.setInt32(kKeyEncoderPadding, encp);
    //     }
    // }
    void *mSeeker = NULL; // FIXME:
    if (mSeeker != NULL) {
        // While it is safe to send the XING/VBRI frame to the decoder, this will
        // result in an extra 1152 samples being output. In addition, the bitrate
        // of the Xing header might not match the rest of the file, which could
        // lead to problems when seeking. The real first frame to decode is after
        // the XING/VBRI frame, so skip there.
        size_t frame_size;
        int sample_rate;
        int num_channels;
        int bitrate;
        GetMPEGAudioFrameSize(header, &frame_size, &sample_rate, &num_channels, &bitrate);
        pos += frame_size;
        if (!Resync(m_dataSource, 0, &pos, &post_id3_pos, &header)) {
            // mInitCheck will remain NO_INIT
            return;
        }
        m_firstFramePos = pos;
        m_fixedHeader   = header;
    }

    size_t frame_size;
    int sample_rate;
    int num_channels;
    int bitrate;
    GetMPEGAudioFrameSize(header, &frame_size, &sample_rate, &num_channels, &bitrate);

    unsigned layer = 4 - ((header >> 17) & 3);

    switch (layer) {
    case 1:
        // mMeta.setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_I);
        LOGI("AUDIO_MPEG_LAYER_I");
        break;
    case 2:
        // mMeta.setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_II);
        LOGI("AUDIO_MPEG_LAYER_II");
        break;
    case 3:
        // mMeta.setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_MPEG);
        LOGI("AUDIO_MPEG");
        break;
    default:
        // TRESPASS();
        break;
    }

    m_spec.sampleRate = static_cast<uint32_t>(sample_rate);
    m_spec.numChannel = static_cast<uint32_t>(num_channels);
    LOGI("sample_rate: %d, bitrate: %d, num_channels: %d", sample_rate, bitrate, num_channels);

    int64_t durationUs;

    if (mSeeker == NULL /* || !mSeeker->getDuration(&durationUs) */) {
        off64_t fileSize;
        if (m_dataSource->getSize(&fileSize) == OK) {
            off64_t dataLength = fileSize - m_firstFramePos;
            if (dataLength > INT64_MAX / 8000LL) {
                // duration would overflow
                durationUs = INT64_MAX;
            } else {
                durationUs = 8000LL * dataLength / bitrate;
            }
        } else {
            durationUs = -1;
        }
    }

    if (durationUs >= 0) {
        m_spec.durationMs = static_cast<uint64_t>(durationUs / 1000);
        LOGI("duration is %lld us", (long long)durationUs);
    }

    normalizeAudioSpec(m_spec);
    m_initCheck = OK;

    // Get iTunes-style gapless info if present.
    // When getting the id3 tag, skip the V1 tags to prevent the source cache
    // from being iterated to the end of the file.
    ID3 id3(m_dataSource, true);
    if (id3.isValid()) {
        ID3::Iterator *com = new ID3::Iterator(id3, "COM");
        if (com->done()) {
            delete com;
            com = new ID3::Iterator(id3, "COMM");
        }
        while (!com->done()) {
            std::string commentDesc;
            std::string commentValue;
            com->getString(&commentDesc, &commentValue);
            const char *desc  = commentDesc.c_str();
            const char *value = commentValue.c_str();

            // first 3 characters are the language, which we don't care about
            if (strlen(desc) > 3 && strcmp(desc + 3, "iTunSMPB") == 0) {

                int32_t delay, padding;
                if (sscanf(value, " %*x %x %x %*x", &delay, &padding) == 2) {
                    // mMeta.setInt32(kKeyEncoderDelay, delay);
                    // mMeta.setInt32(kKeyEncoderPadding, padding);
                    LOGI("delay: %d, padding: %d", delay, padding);
                }
                break;
            }
            com->next();
        }
        delete com;
        com = NULL;
    }

    m_audioCodecID = AUDIO_CODEC_ID_MP3;

    // init 只留 O(1) 摘要：不读整首 payload，也不建任何随包数增长的表。
    // 标称时长仍用首帧码率估算（与旧实现逐位相同，覆盖度基线不动）。
    off64_t fileSize = 0;
    if (m_dataSource->getSize(&fileSize) == OK) {
        m_dataSize = fileSize;
    }
    m_audioEnd = m_dataSize;
    // 尾部 128 字节 ID3v1 尾标不是音频：包序列不把它当坏数据交给解码器，
    // 否则「切在 ID3v1 里」会被误报成坏帧（规格 3.2 P0 要求完全静默）。
    if (m_dataSize >= 128) {
        uint8_t tail[3] = { 0 };
        if (m_dataSource->readAt(m_dataSize - 128, tail, sizeof(tail))
                == static_cast<ssize_t>(sizeof(tail))
            && memcmp(tail, "TAG", 3) == 0) {
            m_audioEnd = m_dataSize - 128;
        }
    }
    if (m_audioEnd < m_firstFramePos) {
        m_audioEnd = m_firstFramePos;
    }
    m_sequenceOffset = m_firstFramePos;
    LOGI("dataSize: %lld, firstFramePos: %lld, audioEnd: %lld", (long long)m_dataSize,
         (long long)m_firstFramePos, (long long)m_audioEnd);
}

MP3FrameReader *MP3Extractor::reader()
{
    if (m_reader == nullptr && m_dataSource != nullptr) {
        m_reader.reset(new MP3FrameReader(m_dataSource, m_firstFramePos, m_audioEnd));
        m_nextPacketIndex = 0;
        m_sequenceOffset  = m_firstFramePos;
    }
    return m_reader.get();
}

AudioBuffer::AudioBufferPtr MP3Extractor::packetAt(size_t index)
{
    // 解复用面在播放期（解码线程第一次拉包时）创建，init 期只有 O(1) 摘要。
    MP3FrameReader *frameReader = reader();
    if (frameReader == nullptr) {
        return nullptr;
    }
    // 只支持顺序拉取。index == 0 是「本段包序列的第一包」：回到本段起点重来。
    if (index == 0) {
        frameReader->seekTo(m_sequenceOffset);
        m_nextPacketIndex = 0;
    } else if (index != m_nextPacketIndex) {
        return nullptr;
    }

    AudioBuffer::AudioBufferPtr packet;
    if (!frameReader->nextPacket(packet)) {
        return nullptr;
    }
    ++m_nextPacketIndex;
    return packet;
}

bool MP3Extractor::seekPacketByte(int64_t offset)
{
    MP3FrameReader *frameReader = reader();
    if (frameReader == nullptr) {
        return false;
    }
    // 0 在调用方语境里是「流头」；mp3 的流头是 ID3v2 之后的第一个音频帧。
    if (offset < m_firstFramePos) {
        offset = m_firstFramePos;
    }
    frameReader->seekTo(offset);
    m_sequenceOffset  = offset;
    m_nextPacketIndex = 0;
    return true;
}

int64_t MP3Extractor::packetBytePosition() const
{
    return m_reader != nullptr ? m_reader->position() : 0;
}

AudioBuffer::AudioBufferPtr MP3Extractor::getMetaData()
{
    // 整块视图（测试量测仪器专用，生产链零调用）：只在被请求时按需读取，
    // 语义与改造前一致——从第一个音频帧到文件尾（含 ID3v1 尾标）。
    if (m_metaBuf == nullptr && m_dataSource != nullptr && m_firstFramePos >= 0
        && m_dataSize > m_firstFramePos) {
        const off64_t length = m_dataSize - m_firstFramePos;
        AudioBuffer::AudioBufferPtr buffer
            = std::make_shared<AudioBuffer>(static_cast<size_t>(length));
        if (m_dataSource->readAt(m_firstFramePos, buffer->data(),
                                 static_cast<size_t>(length))
            == static_cast<ssize_t>(length)) {
            m_metaBuf = buffer;
        } else {
            LOGE("readAt failed for the legacy whole view");
        }
    }
    return m_metaBuf;
}
