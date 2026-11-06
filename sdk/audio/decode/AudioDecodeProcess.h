#pragma once
#include "AudioBuffer.h"
#include "AudioCommon.hpp"
#include "AudioDecode.h"
#include "ExtractorHelper.hpp"
#include "NonCopyable.hpp"
#include <vector>

class AudioDecodeProcess : public NonCopyable, public AudioDecodeCallback {
public:
    // 量测仪器（测试专用）：遍历包工厂逐包拼出整段负载再解码，换算 measuredMs。
    // 生产链（AudioStreamDecoder）走增量供给、不经过本仪器（规格第 3 节 / ADR 0003）；
    // #66 contract 后旧整块视图不再是可选供给形式，本类只有包工厂一条路径。

private:
    ExtractorHelper *m_extractor;
    std::shared_ptr<AudioDecode> m_decode;
    AudioCodecID m_codecID;
    AudioSpec m_spec;
    size_t m_decSize;
    std::vector<AudioBuffer::AudioBufferPtr> m_decBufVec;
    AudioBuffer::AudioBufferPtr m_decBuf;
    sdk_utils::status_t m_initCheck;
    size_t m_payloadPacketCount;

public:
    explicit AudioDecodeProcess(ExtractorHelper *extractor);
    ~AudioDecodeProcess();
    sdk_utils::status_t initCheck() { return m_initCheck; }
    AudioBuffer::AudioBufferPtr getDecodeBuffer(void) { return m_decBuf; }
    AudioSpec getDecodeSpec(void) { return m_spec; }
    // 仪器读数：本次解码从包工厂逐包拿到多少包。
    // 只用于断言「增量路径确实逐包走过」，不参与任何生产判据。
    size_t payloadPacketCount() const { return m_payloadPacketCount; }

protected:
    virtual void onAudioDecodeCallback(AudioDecodeSpec &out) override;
    void mergeDecodeBuffer(AudioBufferPtr &outBuf,
                           std::vector<AudioBuffer::AudioBufferPtr> &inBufVec);

private:
    sdk_utils::status_t init();
};