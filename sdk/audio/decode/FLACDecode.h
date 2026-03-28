#pragma once
#include "AudioBuffer.h"
#include "AudioCommon.hpp"
#include "AudioDecode.h"
#include <FLAC++/decoder.h>
#include <atomic>

class FLACDecode : public FLAC::Decoder::Stream {
private:
    AudioDecodeCallback *m_callback;
    AudioBufferPtr m_inBuf;
    AudioDecodeSpec m_decSpec;
    size_t m_decOffset;
    std::atomic<bool> *m_abortFlag;

public:
    FLACDecode(AudioDecodeCallback *callback);
    ~FLACDecode();
    int decode(const char *data, ssize_t size);
    int decode(AudioBufferPtr &inBuf);
    void setInputBuffer(const AudioBufferPtr &inBuf);
    int processOne();
    bool seekToSample(FLAC__uint64 sample);
    void setAbortFlag(std::atomic<bool> *flag);

protected:
    virtual ::FLAC__StreamDecoderReadStatus read_callback(FLAC__byte buffer[], size_t *bytes);
    virtual ::FLAC__StreamDecoderSeekStatus seek_callback(FLAC__uint64 absolute_byte_offset);
    virtual ::FLAC__StreamDecoderTellStatus tell_callback(FLAC__uint64 *absolute_byte_offset);
    virtual ::FLAC__StreamDecoderLengthStatus length_callback(FLAC__uint64 *stream_length);
    virtual bool eof_callback();
    virtual ::FLAC__StreamDecoderWriteStatus write_callback(const ::FLAC__Frame *frame,
                                                            const FLAC__int32 *const buffer[]);
    virtual void metadata_callback(const ::FLAC__StreamMetadata *metadata);
    virtual void error_callback(::FLAC__StreamDecoderErrorStatus status);
};
