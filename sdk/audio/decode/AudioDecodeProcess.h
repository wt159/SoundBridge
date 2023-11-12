#pragma once
#include "AudioBuffer.h"
#include "AudioDecode.h"
#include "ExtractorHelper.hpp"
#include "NonCopyable.hpp"
#include "AudioCommon.hpp"
#include <vector>

class AudioDecodeProcess : public NonCopyable, public AudioDecodeCallback {
private:
    ExtractorHelper *m_extractor;
    std::shared_ptr<AudioDecode> m_decode;
    AudioCodecID m_codecID;
    AudioSpec m_spec;
    size_t m_decSize;
    std::vector<AudioBuffer::AudioBufferPtr> m_decBufVec;
    AudioBuffer::AudioBufferPtr m_decBuf;
    sdk_utils::status_t m_initCheck;

public:
    AudioDecodeProcess(ExtractorHelper *extractor);
    ~AudioDecodeProcess();
    sdk_utils::status_t initCheck() { return m_initCheck; }
    AudioBuffer::AudioBufferPtr getDecodeBuffer(void) { return m_decBuf; }
    AudioSpec getDecodeSpec(void) { return m_spec; }

protected:
    virtual void onAudioDecodeCallback(AudioDecodeSpec &out) override;
    void mergeDecodeBuffer(AudioBufferPtr &outBuf, std::vector<AudioBuffer::AudioBufferPtr> &inBufVec);

private:
    sdk_utils::status_t init();
};