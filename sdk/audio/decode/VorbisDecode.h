#pragma once
#include "AudioBuffer.h"
#include "AudioCommon.hpp"
#include "AudioDecode.h"
#include <atomic>
#include <vector>
#include <vorbis/vorbisfile.h>

class VorbisDecode {
private:
    AudioDecodeCallback *m_callback;
    AudioDecodeSpec m_decSpec;
    std::atomic<bool> *m_abortFlag;
    const uint8_t *m_data;
    size_t m_size;
    size_t m_pos;
    OggVorbis_File m_vf;
    bool m_vfOpened;
    std::vector<uint8_t> m_interleavedBuf;

    static size_t readCallback(void *ptr, size_t size, size_t nmemb, void *datasource);
    static int seekCallback(void *datasource, ogg_int64_t offset, int whence);
    static int closeCallback(void *datasource);
    static long tellCallback(void *datasource);

    void clearDecodeSpec();
    bool ensureDecodeSpec(int channels, int sampleRate, uint64_t samples);

public:
    VorbisDecode(AudioDecodeCallback *callback);
    ~VorbisDecode();
    bool initVF(const char *data, size_t size);
    int decodeOne();
    bool seekToMs(uint64_t targetMs);
    int decode(const char *data, ssize_t size);
    int decode(AudioBufferPtr &inBuf);
    void setAbortFlag(std::atomic<bool> *flag);

protected:
};
