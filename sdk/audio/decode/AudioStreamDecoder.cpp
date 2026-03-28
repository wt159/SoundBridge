#include "AudioStreamDecoder.h"
#include <algorithm>
#include <chrono>

AudioStreamDecoder::AudioStreamDecoder(AudioRingBuffer *ring, const AudioSpec &spec,
                                       uint64_t durationMs)
    : m_ring(ring)
    , m_spec(spec)
    , m_durationMs(durationMs)
    , m_pcmBuffer(nullptr)
    , m_thread()
    , m_state(StreamDecoderState::IDLE)
    , m_stopRequest(false)
    , m_seekByteOffset(-1)
    , m_consumedBytes(0)
{
}

AudioStreamDecoder::~AudioStreamDecoder()
{
    stop();
}

void AudioStreamDecoder::start(AudioBuffer::AudioBufferPtr pcmBuffer)
{
    stop();
    m_pcmBuffer = pcmBuffer;
    if (m_ring == nullptr || m_pcmBuffer == nullptr) {
        m_state.store(StreamDecoderState::ERROR);
        return;
    }

    m_stopRequest.store(false);
    m_seekByteOffset.store(-1);
    m_consumedBytes.store(0);
    m_state.store(StreamDecoderState::DECODING);
    m_thread = std::thread(&AudioStreamDecoder::threadFunc, this);
}

void AudioStreamDecoder::stop()
{
    m_stopRequest.store(true);
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_stopRequest.store(false);
    m_state.store(StreamDecoderState::IDLE);
}

void AudioStreamDecoder::seekToOffset(size_t byteOffset)
{
    m_seekByteOffset.store(static_cast<int64_t>(byteOffset));
}

StreamDecoderState AudioStreamDecoder::state() const
{
    return m_state.load();
}

uint64_t AudioStreamDecoder::consumedBytes() const
{
    return m_consumedBytes.load();
}

const AudioSpec &AudioStreamDecoder::audioSpec() const
{
    return m_spec;
}

uint64_t AudioStreamDecoder::durationMs() const
{
    return m_durationMs;
}

void AudioStreamDecoder::threadFunc()
{
    if (m_ring == nullptr || m_pcmBuffer == nullptr) {
        m_state.store(StreamDecoderState::ERROR);
        return;
    }

    size_t readOffset  = 0;
    const size_t total = m_pcmBuffer->size();
    m_state.store(StreamDecoderState::DECODING);

    while (!m_stopRequest.load()) {
        int64_t seekOffset = m_seekByteOffset.exchange(-1);
        if (seekOffset >= 0) {
            readOffset = static_cast<size_t>(seekOffset);
            if (readOffset > total) {
                readOffset = total;
            }
            m_ring->reset();
            m_consumedBytes.store(static_cast<uint64_t>(readOffset));
            if (readOffset >= total) {
                m_state.store(StreamDecoderState::EOS);
                break;
            }
            m_state.store(StreamDecoderState::DECODING);
        }

        if (readOffset >= total) {
            m_state.store(StreamDecoderState::EOS);
            break;
        }

        size_t chunk   = std::min(kChunkSize, total - readOffset);
        size_t written = m_ring->write(m_pcmBuffer->data() + readOffset, chunk);
        if (written == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        readOffset += written;
        m_consumedBytes.store(static_cast<uint64_t>(readOffset));
    }

    if (m_stopRequest.load()) {
        m_state.store(StreamDecoderState::IDLE);
    }
}
