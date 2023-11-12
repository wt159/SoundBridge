#pragma once
#include "AudioBuffer.h"
#include "AudioCommon.hpp"
#include "AudioDecode.h"
#include <FLAC++/decoder.h>

class FLACDecode : public FLAC::Decoder::Stream {
private:
    AudioDecodeCallback *m_callback;
    AudioBufferPtr m_inBuf;
    AudioDecodeSpec m_decSpec;
    off64_t m_decOffset;

public:
    FLACDecode(AudioDecodeCallback *callback);
    ~FLACDecode();
    int decode(const char *data, ssize_t size);
    int decode(AudioBufferPtr &inBuf);

protected:
    virtual ::FLAC__StreamDecoderReadStatus read_callback(FLAC__byte buffer[], size_t *bytes);
    virtual ::FLAC__StreamDecoderWriteStatus write_callback(const ::FLAC__Frame *frame,
                                                            const FLAC__int32 *const buffer[]);
    virtual void metadata_callback(const ::FLAC__StreamMetadata *metadata);
    virtual void error_callback(::FLAC__StreamDecoderErrorStatus status);
};
