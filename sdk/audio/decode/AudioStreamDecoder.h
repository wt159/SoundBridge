#pragma once

#include "../resample/AudioResample.h"
#include "AudioCommon.hpp"
#include "AudioDecode.h"
#include "AudioRingBuffer.h"
#include "ErrorUtils.h"
#include "ExtractorHelper.hpp"
#include "NonCopyable.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

enum class StreamDecoderState { IDLE, DECODING, SEEKING, EOS, ERROR };

class AudioStreamDecoder : public NonCopyable, public AudioDecodeCallback {
public:
    AudioStreamDecoder(AudioRingBuffer *ring, const AudioSpec &devSpec);
    ~AudioStreamDecoder();

    sdk_utils::status_t start(ExtractorHelper *extractor);
    void stop();
    void seekToMs(uint64_t targetMs);

    StreamDecoderState state() const;
    uint64_t positionMs() const;
    uint64_t durationMs() const;

protected:
    void onAudioDecodeCallback(AudioDecodeSpec &out) override;

private:
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

    std::shared_ptr<AudioResample> m_resample;
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
};
