#include "FLACDecode.h"
#include "LogWrapper.h"
#include <cstring>

#define LOG_TAG "FLACDecode"

FLACDecode::FLACDecode(AudioDecodeCallback *callback)
    : FLAC::Decoder::Stream()
    , m_callback(callback)
    , m_inBuf(nullptr)
    , m_dataSource(nullptr)
    , m_audioDataOffset(0)
    , m_dataSourceSize(0)
    , m_decSpec()
    , m_decOffset(0)
    , m_abortFlag(nullptr)
{
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
    setInputBuffer(inBuf);
    return this->process_until_end_of_stream() == true ? 0 : -1;
}

bool FLACDecode::initFromDataSource(DataSourceBase *src, off64_t audioOffset, off64_t dataSize)
{
    m_dataSource      = src;
    m_audioDataOffset = audioOffset;
    m_dataSourceSize  = dataSize - audioOffset;
    m_decOffset       = 0;
    m_inBuf           = nullptr;
    return true;
}

void FLACDecode::setInputBuffer(const AudioBufferPtr &inBuf)
{
    m_inBuf           = inBuf;
    m_decOffset       = 0;
    m_dataSource      = nullptr;
    m_audioDataOffset = 0;
    m_dataSourceSize  = 0;
}

int FLACDecode::processOne()
{
    if (m_inBuf == nullptr && m_dataSource == nullptr) {
        return -1;
    }

    if (this->get_state() == FLAC__STREAM_DECODER_END_OF_STREAM) {
        return 0;
    }

    bool ret = this->process_single();
    if (!ret) {
        return (this->get_state() == FLAC__STREAM_DECODER_END_OF_STREAM) ? 0 : -1;
    }
    return (this->get_state() == FLAC__STREAM_DECODER_END_OF_STREAM) ? 0 : 1;
}

bool FLACDecode::seekToSample(FLAC__uint64 sample)
{
    if (m_inBuf == nullptr && m_dataSource == nullptr) {
        return false;
    }
    return this->seek_absolute(sample);
}

void FLACDecode::setAbortFlag(std::atomic<bool> *flag)
{
    m_abortFlag = flag;
}

::FLAC__StreamDecoderReadStatus FLACDecode::read_callback(FLAC__byte buffer[], size_t *bytes)
{
    if (m_dataSource != nullptr) {
        off64_t remaining = m_dataSourceSize - static_cast<off64_t>(m_decOffset);
        if (remaining <= 0) {
            *bytes = 0;
            return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
        }
        if (static_cast<off64_t>(*bytes) > remaining) {
            *bytes = static_cast<size_t>(remaining);
        }
        ssize_t got = m_dataSource->readAt(m_audioDataOffset + static_cast<off64_t>(m_decOffset),
                                           buffer, *bytes);
        if (got <= 0) {
            *bytes = 0;
            return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
        }
        *bytes       = static_cast<size_t>(got);
        m_decOffset += *bytes;
        return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
    }

    if (m_inBuf == nullptr) {
        return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    }

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

::FLAC__StreamDecoderSeekStatus FLACDecode::seek_callback(FLAC__uint64 absolute_byte_offset)
{
    if (m_dataSource != nullptr) {
        if (absolute_byte_offset > static_cast<FLAC__uint64>(m_dataSourceSize)) {
            return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
        }
        m_decOffset = static_cast<size_t>(absolute_byte_offset);
        return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
    }

    if (m_inBuf == nullptr) {
        return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
    }
    if (absolute_byte_offset > static_cast<FLAC__uint64>(m_inBuf->size())) {
        return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
    }
    m_decOffset = static_cast<size_t>(absolute_byte_offset);
    return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
}

::FLAC__StreamDecoderTellStatus FLACDecode::tell_callback(FLAC__uint64 *absolute_byte_offset)
{
    if (absolute_byte_offset == nullptr) {
        return FLAC__STREAM_DECODER_TELL_STATUS_ERROR;
    }

    if (m_dataSource != nullptr) {
        *absolute_byte_offset = static_cast<FLAC__uint64>(m_decOffset);
        return FLAC__STREAM_DECODER_TELL_STATUS_OK;
    }

    if (m_inBuf == nullptr) {
        return FLAC__STREAM_DECODER_TELL_STATUS_ERROR;
    }

    *absolute_byte_offset = static_cast<FLAC__uint64>(m_decOffset);
    return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

::FLAC__StreamDecoderLengthStatus FLACDecode::length_callback(FLAC__uint64 *stream_length)
{
    if (stream_length == nullptr) {
        return FLAC__STREAM_DECODER_LENGTH_STATUS_ERROR;
    }

    if (m_dataSource != nullptr) {
        *stream_length = static_cast<FLAC__uint64>(m_dataSourceSize);
        return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
    }

    if (m_inBuf == nullptr) {
        return FLAC__STREAM_DECODER_LENGTH_STATUS_ERROR;
    }

    *stream_length = static_cast<FLAC__uint64>(m_inBuf->size());
    return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
}

bool FLACDecode::eof_callback()
{
    if (m_dataSource != nullptr) {
        return m_decOffset >= static_cast<size_t>(m_dataSourceSize);
    }

    if (m_inBuf == nullptr) {
        return true;
    }
    return m_decOffset >= m_inBuf->size();
}

::FLAC__StreamDecoderWriteStatus FLACDecode::write_callback(const ::FLAC__Frame *frame,
                                                            const FLAC__int32 *const buffer[])
{
    if (m_abortFlag != nullptr && m_abortFlag->load()) {
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }

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
        LOGI("numChannel: %d, bitsPerSample: %d, bytesPerSample: %d, sampleRate: %d, samples: %llu",
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
    for (uint32_t i = 0; i < frame->header.blocksize; i++) {
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
        LOGI("numChannel: %d, bitsPerSample: %d, bytesPerSample: %d, sampleRate: %d, samples: %llu",
             m_decSpec.spec.numChannel, m_decSpec.spec.bitsPerSample, m_decSpec.spec.bytesPerSample,
             m_decSpec.spec.sampleRate, m_decSpec.spec.samples);
    }
}

void FLACDecode::error_callback(::FLAC__StreamDecoderErrorStatus status)
{
    LOGE("error_callback: %s", FLAC__StreamDecoderErrorStatusString[status]);
}
