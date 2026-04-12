#pragma once

#include "../resample/AudioResample.h"
#include "AudioCommon.hpp"
#include "AudioDecode.h"
#include "AudioRingBuffer.h"
#include "ErrorUtils.h"
#include "ExtractorHelper.hpp"
#include "Lazy.hpp"
#include "NonCopyable.hpp"
#include "Optional.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

// Forward declarations
class FLACDecode;
class VorbisDecode;

enum class DecodeResult {
    OK    = 0,
    WAIT  = 1, // 需等待 (ring 满 / 数据不足)
    EOS   = 2,
    ERROR = -1,
    ABORT = -2
};

enum class StreamDecoderState { IDLE, DECODING, SEEKING, EOS, ERROR };

struct DecodeOptions {
    size_t minWriteSpace = 0; // 0 = 自动计算
    bool autoDecode      = true; // 默认开启自动解码
};

class AudioStreamDecoder : public NonCopyable, public AudioDecodeCallback {
public:
    AudioStreamDecoder(AudioRingBuffer *ring, const AudioSpec &devSpec);
    ~AudioStreamDecoder();

    sdk_utils::status_t start(ExtractorHelper *extractor);
    sdk_utils::status_t start(ExtractorHelper *extractor, const DecodeOptions &opts);
    void stop();
    void seekToMs(uint64_t targetMs);

    StreamDecoderState state() const;
    uint64_t positionMs() const;
    uint64_t durationMs() const;
    bool canDecode() const;
    DecodeResult decodeNext(const DecodeOptions &opts = DecodeOptions());

protected:
    void onAudioDecodeCallback(AudioDecodeSpec &out) override;

private:
    struct DecoderAdapter {
        enum Type { NONE, FLAC, VORBIS, FFMPEG } type;
        union {
            FLACDecode *flac;
            VorbisDecode *vorbis;
            void *ffmpeg;
        };

        DecoderAdapter()
            : type(NONE)
            , flac(nullptr)
        {
        }

        Type getType() const { return type; }
        bool isFlac() const { return type == FLAC; }
        bool isVorbis() const { return type == VORBIS; }
        bool isFfmpg() const { return type == FFMPEG; }
    };

    size_t calculateMinWriteSpace(AudioCodecID codecId) const;

    void threadFunc();
    int runDecode();
    bool writeDecodedBytes(const char *pcm, size_t pcmSize);

private:
    AudioRingBuffer *m_ring;
    AudioSpec m_devSpec;
    AudioSpec m_srcSpec;
    ExtractorHelper *m_extractor;
    AudioCodecID m_codecID;
    uint64_t m_durationMs;

    Optional<Lazy<std::shared_ptr<AudioResample>>> m_lazyResample;
    std::vector<char> m_interleaveBuf;
    std::vector<char> m_resampleBuf;

    std::atomic<StreamDecoderState> m_state;
    std::atomic<bool> m_stopRequest;
    std::atomic<int64_t> m_seekRequestMs;
    size_t m_seekDiscardBytes;
    size_t m_processedBytes;
    std::atomic<uint64_t> m_consumedBytes;
    uint64_t m_bytesPerMs;
    std::atomic<bool> m_abortDecode;

    std::thread m_thread;

    DecoderAdapter m_adapter;
    DecodeOptions m_options;
};
