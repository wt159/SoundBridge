#include "LogWrapper.h"
#include "VorbisDecode.h"
#include <cmath>
#include <cstring>

#define LOG_TAG "VorbisDecode"

VorbisDecode::VorbisDecode(AudioDecodeCallback *callback)
    : m_callback(callback)
{
}

VorbisDecode::~VorbisDecode()
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

int VorbisDecode::decode(const char *data, ssize_t size)
{
    char *buffer;
    int bytes, ret = 0;
    off64_t offset = 0;

    auto readFormData = [&](char *buf, int len) {
        off64_t remainSize = size - offset;
        if (remainSize >= len) {
            memcpy(buf, data + offset, len);
            offset += len;
            return len;
        }
        memcpy(buf, data + offset, remainSize);
        offset += remainSize;
        return (int)remainSize;
    };

    ogg_sync_init(&oy);

    while (1) { /* we repeat if the bitstream is chained */
        int eos = 0;
        int i;

        /* submit a 4k block to libvorbis' Ogg layer */
        buffer = ogg_sync_buffer(&oy, 4096);
        bytes  = readFormData(buffer, 4096);
        ogg_sync_wrote(&oy, bytes);

        /* Get the first page. */
        if (ogg_sync_pageout(&oy, &og) != 1) {
            if (bytes < 4096)
                break;
            LOGE("Input does not appear to be an Ogg bitstream.");
            return -1;
        }

        /* Get the serial number and set up the rest of decode. */
        /* serialno first; use it to set up a logical stream */
        ogg_stream_init(&os, ogg_page_serialno(&og));

        vorbis_info_init(&vi);
        vorbis_comment_init(&vc);
        if (ogg_stream_pagein(&os, &og) < 0) {
            LOGE("Error reading first page of Ogg bitstream data.");
            return -1;
        }

        if (ogg_stream_packetout(&os, &op) != 1) {
            LOGE("Error reading initial header packet.");
            return -1;
        }

        if (vorbis_synthesis_headerin(&vi, &vc, &op) < 0) {
            LOGE("This Ogg bitstream does not contain Vorbis "
                 "audio data.");
            return -1;
        }

        i = 0;
        while (i < 2) {
            while (i < 2) {
                int result = ogg_sync_pageout(&oy, &og);
                if (result == 0)
                    break;
                if (result == 1) {
                    ogg_stream_pagein(&os, &og);
                    while (i < 2) {
                        result = ogg_stream_packetout(&os, &op);
                        if (result == 0)
                            break;
                        if (result < 0) {
                            LOGE("Corrupt secondary header.  Exiting.");
                            return -1;
                        }
                        result = vorbis_synthesis_headerin(&vi, &vc, &op);
                        if (result < 0) {
                            LOGE("Corrupt secondary header.  Exiting.");
                            return -1;
                        }
                        i++;
                    }
                }
            }
            /* no harm in not checking before adding more */
            buffer = ogg_sync_buffer(&oy, 4096);
            bytes  = readFormData(buffer, 4096);
            if (bytes == 0 && i < 2) {
                LOGE("End of file before finding all Vorbis headers!");
                return -2;
            }
            ogg_sync_wrote(&oy, bytes);
        }

        /* Throw the comments plus a few lines about the bitstream we're
           decoding */
        {
            char **ptr = vc.user_comments;
            while (*ptr) {
                LOGD("%s", *ptr);
                ++ptr;
            }
            LOGI("Bitstream is %d channel, %ldHz", vi.channels, vi.rate);
            LOGI("Encoded by: %s", vc.vendor);
        }

        if (vorbis_synthesis_init(&vd, &vi) == 0) { /* central decode state */
            vorbis_block_init(&vd, &vb);

            /* The rest is just a straight decode loop until end of stream */
            while (!eos) {
                while (!eos) {
                    int result = ogg_sync_pageout(&oy, &og);
                    if (result == 0)
                        break; /* need more data */
                    if (result < 0) { /* missing or corrupt data at this page position */
                        LOGE("Corrupt or missing data in bitstream; continuing...");
                    } else {
                        ogg_stream_pagein(&os, &og); /* can safely ignore errors at
                                                        this point */
                        while (1) {
                            result = ogg_stream_packetout(&os, &op);

                            if (result == 0)
                                break; /* need more data */
                            if (result < 0) { /* missing or corrupt data at this page position */
                                /* no reason to complain; already complained above */
                            } else {
                                /* we have a packet.  Decode it */
                                float **pcm;
                                int samples;

                                if (vorbis_synthesis(&vb, &op) == 0) /* test for success! */
                                    vorbis_synthesis_blockin(&vd, &vb);
                                /*

                                **pcm is a multichannel float vector.  In stereo, for
                                example, pcm[0] is left, and pcm[1] is right.  samples is
                                the size of each channel.  Convert the float values
                                (-1.<=range<=1.) to whatever PCM format and write it out */

                                while ((samples = vorbis_synthesis_pcmout(&vd, &pcm)) > 0) {
                                    int j;
                                    int clipflag = 0;
                                    int bout     = samples;

                                    if (m_decSpec.lineSize == nullptr) {
                                        m_decSpec.spec.sampleRate    = vi.rate;
                                        m_decSpec.spec.bitsPerSample = 16;
                                        m_decSpec.spec.bytesPerSample
                                            = m_decSpec.spec.bitsPerSample >> 3;
                                        m_decSpec.spec.format = getAudioFormatByBitPreSample(
                                            m_decSpec.spec.bitsPerSample);
                                        m_decSpec.spec.numChannel = vi.channels;
                                        m_decSpec.lineData
                                            = new uint8_t *[m_decSpec.spec.numChannel];
                                        m_decSpec.lineSize = new int[m_decSpec.spec.numChannel];
                                        for (int ch = 0; ch < m_decSpec.spec.numChannel; ch++) {
                                            m_decSpec.lineSize[ch] = 0;
                                            m_decSpec.lineData[ch] = nullptr;
                                        }
                                    }
                                    if (m_decSpec.spec.samples != (uint64_t)bout) {
                                        m_decSpec.spec.samples = bout;
                                        for (int ch = 0; ch < m_decSpec.spec.numChannel; ch++) {
                                            if (m_decSpec.lineData[ch] != nullptr)
                                                delete[] m_decSpec.lineData[ch];
                                            m_decSpec.lineSize[ch] = m_decSpec.spec.samples
                                                * m_decSpec.spec.bytesPerSample;
                                            m_decSpec.lineData[ch]
                                                = new uint8_t[m_decSpec.lineSize[ch]];
                                        }
                                    }

                                    /* convert floats to 16 bit signed ints (host order) and
                                       interleave */
                                    for (i = 0; i < vi.channels; i++) {
                                        ogg_int16_t *ptr = (ogg_int16_t *)m_decSpec.lineData[i] + i;
                                        float *mono      = pcm[i];
                                        for (j = 0; j < bout; j++) {
#if 1
                                            int val = floor(mono[j] * 32767.f + .5f);
#else /* optional dither */
                                            int val = mono[j] * 32767.f + drand48() - 0.5f;
#endif
                                            /* might as well guard against clipping */
                                            if (val > 32767) {
                                                val      = 32767;
                                                clipflag = 1;
                                            }
                                            if (val < -32768) {
                                                val      = -32768;
                                                clipflag = 1;
                                            }
                                            *ptr = val;
                                        }
                                    }

                                    if (clipflag) {
                                        LOGE("Clipping in frame %ld", (long)(vd.sequence));
                                    }

                                    m_callback->onAudioDecodeCallback(m_decSpec);
                                    vorbis_synthesis_read(&vd, bout);
                                }
                            }
                        }
                        if (ogg_page_eos(&og))
                            eos = 1;
                    }
                }
                if (!eos) {
                    buffer = ogg_sync_buffer(&oy, 4096);
                    bytes  = fread(buffer, 1, 4096, stdin);
                    ogg_sync_wrote(&oy, bytes);
                    if (bytes == 0)
                        eos = 1;
                }
            }
            vorbis_block_clear(&vb);
            vorbis_dsp_clear(&vd);
        } else {
            LOGE("Error: Corrupt header during playback initialization.");
        }

        ogg_stream_clear(&os);
        vorbis_comment_clear(&vc);
        vorbis_info_clear(&vi); /* must be called last */
    }

    /* OK, clean up the framer */
    ogg_sync_clear(&oy);
    return ret;
}

int VorbisDecode::decode(AudioBufferPtr &inBuf)
{
    return decode(inBuf->data(), inBuf->size());
}
