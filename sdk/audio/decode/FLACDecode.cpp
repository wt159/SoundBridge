#include "FLACDecode.h"
#include "LogWrapper.h"
#include <cstring>

#define LOG_TAG "FLACDecode"

FLACDecode::FLACDecode(AudioDecodeCallback *callback)
    : FLAC::Decoder::Stream()
    , m_callback(callback)
    , m_inBuf(nullptr)
    , m_decSpec()
    , m_decOffset(0)
{
    memset(&m_decSpec, 0, sizeof(m_decSpec));
    ::FLAC__StreamDecoderInitStatus init_status = this->init();
    if (init_status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        LOGE("initializing decoder: %s", FLAC__StreamDecoderInitStatusString[init_status]);
    }
    this->set_md5_checking(true);
}

FLACDecode::~FLACDecode()
{
    if (m_decSpec.lineData != nullptr) {
        for (int ch = 0; ch < m_decSpec.spec.numChannel; ch++) {
            delete[] m_decSpec.lineData[ch];
            m_decSpec.lineData[ch] = nullptr;
        }
        delete[] m_decSpec.lineData;
        m_decSpec.lineData = nullptr;
    }
    if (m_decSpec.lineSize != nullptr) {
        delete[] m_decSpec.lineSize;
        m_decSpec.lineSize = nullptr;
    }
}

int FLACDecode::decode(const char *data, ssize_t size)
{
    m_inBuf     = std::make_shared<AudioBuffer>(size);
    m_decOffset = 0;
    memcpy(m_inBuf->data(), data, size);
    return this->process_until_end_of_stream() == true ? 0 : -1;
}

int FLACDecode::decode(AudioBufferPtr &inBuf)
{
    m_inBuf     = inBuf;
    m_decOffset = 0;
    return this->process_until_end_of_stream() == true ? 0 : -1;
}

::FLAC__StreamDecoderReadStatus FLACDecode::read_callback(FLAC__byte buffer[], size_t *bytes)
{
    ::FLAC__StreamDecoderReadStatus status = ::FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
    if (*bytes > 0) {
        size_t size = m_inBuf->size() - m_decOffset;
        if (*bytes > size)
            *bytes = size;
        memcpy(buffer, m_inBuf->data() + m_decOffset, *bytes);
        m_decOffset += *bytes;
        if (*bytes == 0)
            status = FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
        else
            status = FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
    } else
        status = FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    return status;
}

::FLAC__StreamDecoderWriteStatus FLACDecode::write_callback(const ::FLAC__Frame *frame,
                                                            const FLAC__int32 *const buffer[])
{
    ::FLAC__StreamDecoderWriteStatus status = ::FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    if (frame->header.number.sample_number == 0) {
        m_decSpec.spec.numChannel     = this->get_channels();
        m_decSpec.spec.bitsPerSample  = this->get_bits_per_sample();
        m_decSpec.spec.bytesPerSample = m_decSpec.spec.bitsPerSample >> 3;
        m_decSpec.spec.format         = getAudioFormatByBitPreSample(m_decSpec.spec.bitsPerSample);
        m_decSpec.spec.sampleRate     = this->get_sample_rate();
        m_decSpec.lineData            = new uint8_t *[m_decSpec.spec.numChannel];
        m_decSpec.lineSize            = new int[m_decSpec.spec.numChannel];
        for (int ch = 0; ch < m_decSpec.spec.numChannel; ch++) {
            m_decSpec.lineData[ch] = nullptr;
        }
        LOGI("numChannel: %d, bitsPerSample: %d, bytesPerSample: %d, sampleRate: %d, samples: %d",
             m_decSpec.spec.numChannel, m_decSpec.spec.bitsPerSample, m_decSpec.spec.bytesPerSample,
             m_decSpec.spec.sampleRate, m_decSpec.spec.samples);
    }
    if (m_decSpec.spec.samples != frame->header.blocksize) {
        m_decSpec.spec.samples = frame->header.blocksize;
        for (int ch = 0; ch < m_decSpec.spec.numChannel; ch++) {
            if (m_decSpec.lineData[ch] != nullptr)
                delete[] m_decSpec.lineData[ch];
            m_decSpec.lineSize[ch] = m_decSpec.spec.samples * m_decSpec.spec.bytesPerSample;
            m_decSpec.lineData[ch] = new uint8_t[m_decSpec.lineSize[ch]];
        }
    }
    for (int i = 0; i < frame->header.blocksize; i++) {
        for (int ch = 0; ch < m_decSpec.spec.numChannel; ch++) {
            memcpy(m_decSpec.lineData[ch] + i * m_decSpec.spec.bytesPerSample, buffer[ch] + i,
                   m_decSpec.spec.bytesPerSample);
        }
    }
    m_callback->onAudioDecodeCallback(m_decSpec);
    return status;
}

void FLACDecode::metadata_callback(const ::FLAC__StreamMetadata *metadata)
{
    if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
        m_decSpec.spec.numChannel     = metadata->data.stream_info.channels;
        m_decSpec.spec.bitsPerSample  = metadata->data.stream_info.bits_per_sample;
        m_decSpec.spec.bytesPerSample = metadata->data.stream_info.bits_per_sample >> 3;
        m_decSpec.spec.sampleRate     = metadata->data.stream_info.sample_rate;
        m_decSpec.spec.samples        = metadata->data.stream_info.max_blocksize;
        LOGI("numChannel: %d, bitsPerSample: %d, bytesPerSample: %d, sampleRate: %d, samples: %d",
             m_decSpec.spec.numChannel, m_decSpec.spec.bitsPerSample, m_decSpec.spec.bytesPerSample,
             m_decSpec.spec.sampleRate, m_decSpec.spec.samples);
    }
}

void FLACDecode::error_callback(::FLAC__StreamDecoderErrorStatus status)
{
    LOGE("error_callback: %s", FLAC__StreamDecoderErrorStatusString[status]);
}
