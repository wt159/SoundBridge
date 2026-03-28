#pragma once

#include "AudioBuffer.h"
#include "AudioCommon.hpp"
#include "AudioRingBuffer.h"
#include "NonCopyable.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

enum class StreamDecoderState { IDLE, DECODING, EOS, ERROR };

class AudioStreamDecoder : public NonCopyable {
public:
    AudioStreamDecoder(AudioRingBuffer *ring, const AudioSpec &spec, uint64_t durationMs);
    ~AudioStreamDecoder();

    void start(AudioBuffer::AudioBufferPtr pcmBuffer);
    void stop();
    void seekToOffset(size_t byteOffset);

    StreamDecoderState state() const;
    uint64_t consumedBytes() const;
    const AudioSpec &audioSpec() const;
    uint64_t durationMs() const;

private:
    void threadFunc();

private:
    AudioRingBuffer *m_ring;
    AudioSpec m_spec;
    uint64_t m_durationMs;
    AudioBuffer::AudioBufferPtr m_pcmBuffer;
    std::thread m_thread;
    std::atomic<StreamDecoderState> m_state;
    std::atomic<bool> m_stopRequest;
    std::atomic<int64_t> m_seekByteOffset;
    std::atomic<uint64_t> m_consumedBytes;

    static constexpr size_t kChunkSize = 4096;
};
