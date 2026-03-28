#pragma once
#include "AudioBuffer.h"
#include "AudioCommon.hpp"
#include "AudioDecode.h"
#include <atomic>
#include <vorbis/codec.h>

class VorbisDecode {
private:
    AudioDecodeCallback *m_callback;
    AudioBufferPtr m_inBuf;
    AudioDecodeSpec m_decSpec;
    off64_t m_decOffset;
    std::atomic<bool> *m_abortFlag;
    ogg_sync_state oy;
    ogg_stream_state os;
    ogg_page og;
    ogg_packet op;
    vorbis_info vi;
    vorbis_comment vc;
    vorbis_dsp_state vd;
    vorbis_block vb;

public:
    VorbisDecode(AudioDecodeCallback *callback);
    ~VorbisDecode();
    int decode(const char *data, ssize_t size);
    int decode(AudioBufferPtr &inBuf);
    void setAbortFlag(std::atomic<bool> *flag);

protected:
};
