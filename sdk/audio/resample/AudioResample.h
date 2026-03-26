#pragma once
#include "ErrorUtils.h"
#include <AudioCommon.hpp>
#include <NonCopyable.hpp>
#include <memory>

class AudioResample : public NonCopyable {
public:
    AudioResample();
    AudioResample(AudioSpec &in, AudioSpec &out);
    ~AudioResample();
    void init(AudioSpec &in, AudioSpec &out);
    sdk_utils::status_t initCheck();
    /**
     * @brief Resampling function
     *
     * @param in    input audio data
     * @param inLen input audio data length
     * @param out   output audio data
     * @param outLen [in]  output buffer size, [out] actual output data length (may be larger than
     * calculated)
     * @return int 0: success, other: failed
     * @version 0.1
     * @author wtp (wtp0727@gmail.com)
     * @date 2023-07-30
     * @copyright Copyright (c) 2023
     */
    int resample(void *in, size_t inLen, void *out, size_t *outLen);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};