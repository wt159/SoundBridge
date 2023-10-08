#include "ByteUtils.h"
#include "ID3.h"
#include "LogWrapper.h"
#include "MP3Extractor.h"
#include <cstring>

#define LOG_TAG   "MP3Extractor"

#define __unused __attribute__((unused))

using namespace sdk_utils;
// Everything must match except for
// protection, bitrate, padding, private bits, mode, mode extension,
// copyright bit, original bit and emphasis.
// Yes ... there are things that must indeed match...
static const uint32_t kMask = 0xfffe0c00;

static bool GetMPEGAudioFrameSize(uint32_t header, size_t *frame_size,
                                  int *out_sampling_rate = NULL, int *out_channels = NULL,
                                  int *out_bitrate = NULL, int *out_num_samples = NULL)
{
    *frame_size = 0;

    if (out_sampling_rate) {
        *out_sampling_rate = 0;
    }

    if (out_channels) {
        *out_channels = 0;
    }

    if (out_bitrate) {
        *out_bitrate = 0;
    }

    if (out_num_samples) {
        *out_num_samples = 1152;
    }

    if ((header & 0xffe00000) != 0xffe00000) {
        return false;
    }

    unsigned version = (header >> 19) & 3;

    if (version == 0x01) {
        return false;
    }

    unsigned layer = (header >> 17) & 3;

    if (layer == 0x00) {
        return false;
    }

    unsigned protection __unused = (header >> 16) & 1;

    unsigned bitrate_index = (header >> 12) & 0x0f;

    if (bitrate_index == 0 || bitrate_index == 0x0f) {
        // Disallow "free" bitrate.
        return false;
    }

    unsigned sampling_rate_index = (header >> 10) & 3;

    if (sampling_rate_index == 3) {
        return false;
    }

    static const int kSamplingRateV1[] = { 44100, 48000, 32000 };
    int sampling_rate                  = kSamplingRateV1[sampling_rate_index];
    if (version == 2 /* V2 */) {
        sampling_rate /= 2;
    } else if (version == 0 /* V2.5 */) {
        sampling_rate /= 4;
    }

    unsigned padding = (header >> 9) & 1;

    if (layer == 3) {
        // layer I

        static const int kBitrateV1[]
            = { 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448 };

        static const int kBitrateV2[]
            = { 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256 };

        int bitrate = (version == 3 /* V1 */) ? kBitrateV1[bitrate_index - 1]
                                              : kBitrateV2[bitrate_index - 1];

        if (out_bitrate) {
            *out_bitrate = bitrate;
        }

        *frame_size = (12000 * bitrate / sampling_rate + padding) * 4;

        if (out_num_samples) {
            *out_num_samples = 384;
        }
    } else {
        // layer II or III

        static const int kBitrateV1L2[]
            = { 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384 };

        static const int kBitrateV1L3[]
            = { 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 };

        static const int kBitrateV2[]
            = { 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 };

        int bitrate;
        if (version == 3 /* V1 */) {
            bitrate = (layer == 2 /* L2 */) ? kBitrateV1L2[bitrate_index - 1]
                                            : kBitrateV1L3[bitrate_index - 1];

            if (out_num_samples) {
                *out_num_samples = 1152;
            }
        } else {
            // V2 (or 2.5)

            bitrate = kBitrateV2[bitrate_index - 1];
            if (out_num_samples) {
                *out_num_samples = (layer == 1 /* L3 */) ? 576 : 1152;
            }
        }

        if (out_bitrate) {
            *out_bitrate = bitrate;
        }

        if (version == 3 /* V1 */) {
            *frame_size = 144000 * bitrate / sampling_rate + padding;
        } else {
            // V2 or V2.5
            size_t tmp  = (layer == 1 /* L3 */) ? 72000 : 144000;
            *frame_size = tmp * bitrate / sampling_rate + padding;
        }
    }

    if (out_sampling_rate) {
        *out_sampling_rate = sampling_rate;
    }

    if (out_channels) {
        int channel_mode = (header >> 6) & 3;

        *out_channels = (channel_mode == 3) ? 1 : 2;
    }

    return true;
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

            *inout_pos += len;

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

        if (match_header != 0 && (header & kMask) != (match_header & kMask)) {
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

            if ((test_header & kMask) != (header & kMask)) {
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
    , m_firstFramePos(-1)
    , m_fixedHeader(0)
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

    // mMeta.setInt32(kKeySampleRate, sample_rate);
    // mMeta.setInt32(kKeyBitRate, bitrate * 1000);
    // mMeta.setInt32(kKeyChannelCount, num_channels);
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
        LOGI("duration is %lld us", (long long)durationUs);
    }

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

    off64_t fileSize = 0;
    m_dataSource->getSize(&fileSize);
    fileSize -= m_firstFramePos;
    m_metaBuf = std::make_shared<AudioBuffer>(fileSize);
    if(m_dataSource->readAt(m_firstFramePos, m_metaBuf->data(), fileSize) < 0) {
        LOGE("readAt failed");
    }
    LOGI("m_metaBuf->data(): %p, size: %llu", m_metaBuf->data(), m_metaBuf->size());
}