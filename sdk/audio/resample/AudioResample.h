#pragma once
#include <NonCopyable.hpp>
#include <AudioCommon.hpp>
#include <memory>

class AudioResample : public NonCopyable {
public:
    AudioResample();
    AudioResample(AudioSpec &in, AudioSpec &out);
    ~AudioResample();
    void init(AudioSpec &in, AudioSpec &out);

    /**
     * @brief Resampling function
     * 
     * @param in    input audio data
     * @param inLen input audio data length
     * @param out   output audio data
     * @param outLen return output audio data length(The length may be larger than the calculated output length)
     * @return int 0: success, other: failed
     * @version 0.1
     * @author wtp (wtp0727@gmail.com)
     * @date 2023-07-30
     * @copyright Copyright (c) 2023
     */
    int resample(void *in, int inLen, void *out, int *outLen);
private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};