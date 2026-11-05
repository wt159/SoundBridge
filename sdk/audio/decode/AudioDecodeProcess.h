#pragma once
#include "AudioBuffer.h"
#include "AudioCommon.hpp"
#include "AudioDecode.h"
#include "ExtractorHelper.hpp"
#include "NonCopyable.hpp"
#include <vector>

class AudioDecodeProcess : public NonCopyable, public AudioDecodeCallback {
public:
    // 负载供给形式（expand 阶段，#57 / ADR 0003）：
    //   WholeView       —— 旧形式，默认：整块视图（getPacketizedMetaData() / getMetaData()）；
    //   PacketIteration —— 新形式：由 PacketCursor 逐包供给，拼出同一段负载再解码。
    // 两者对同一曲目必须解出同样的 PCM；生产链（AudioStreamDecoder）本票不迁移，与本仪器无关。
    enum class PayloadSupply { WholeView, PacketIteration };

private:
    ExtractorHelper *m_extractor;
    std::shared_ptr<AudioDecode> m_decode;
    AudioCodecID m_codecID;
    AudioSpec m_spec;
    size_t m_decSize;
    std::vector<AudioBuffer::AudioBufferPtr> m_decBufVec;
    AudioBuffer::AudioBufferPtr m_decBuf;
    sdk_utils::status_t m_initCheck;
    PayloadSupply m_supply;
    size_t m_payloadPacketCount;

public:
    explicit AudioDecodeProcess(ExtractorHelper *extractor,
                                PayloadSupply supply = PayloadSupply::WholeView);
    ~AudioDecodeProcess();
    sdk_utils::status_t initCheck() { return m_initCheck; }
    AudioBuffer::AudioBufferPtr getDecodeBuffer(void) { return m_decBuf; }
    AudioSpec getDecodeSpec(void) { return m_spec; }
    // 仪器读数：本次解码从供给路径拿到多少包（整块视图按 0/1 计）。
    // 只用于断言「新路径确实逐包走过」，不参与任何生产判据。
    size_t payloadPacketCount() const { return m_payloadPacketCount; }

protected:
    virtual void onAudioDecodeCallback(AudioDecodeSpec &out) override;
    void mergeDecodeBuffer(AudioBufferPtr &outBuf,
                           std::vector<AudioBuffer::AudioBufferPtr> &inBufVec);

private:
    sdk_utils::status_t init();
};