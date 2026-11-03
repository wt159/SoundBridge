#pragma once

#include "../../ErrorCode.hpp"
#include "../resample/AudioResample.h"
#include "AudioCommon.hpp"
#include "AudioDecode.h"
#include "AudioRingBuffer.h"
#include "ErrorUtils.h"
#include "ExtractorHelper.hpp"
#include "Lazy.hpp"
#include "NonCopyable.hpp"
#include "Optional.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Forward declarations
class FLACDecode;
class VorbisDecode;

enum class StreamDecoderState { IDLE, DECODING, SEEKING, EOS, ERROR };

class AudioStreamDecoder : public NonCopyable, public AudioDecodeCallback {
public:
    AudioStreamDecoder(AudioRingBuffer *ring, const AudioSpec &devSpec);
    ~AudioStreamDecoder();

    sdk_utils::status_t start(ExtractorHelper *extractor);
    // startThread=false 只做初始化（供只检查状态/生命周期的场合），不会产出任何数据：
    // 同步解码入口已按 #18 删除，解码只能由 threadFunc 驱动。
    sdk_utils::status_t start(ExtractorHelper *extractor, bool startThread);
    void stop();
    void seekToMs(uint64_t targetMs);

    StreamDecoderState state() const;
    uint64_t positionMs() const;
    uint64_t durationMs() const;
    bool canDecode() const;

    // 进入 ERROR 之前先写原因：错误码只表达原因，处置由调用方按 ErrorInfo 决定
    // （docs/adr/0001-error-code-expresses-cause.md）。未进入 ERROR 时返回 Ok。
    sdk::ErrorCode errorCode() const;
    std::string errorDetail() const;

    // 刻意不提供同步的 decodeNext()（#18）。它曾对三类 codec 有三套语义：
    // FLAC/Vorbis 每次调用新建解码器 → 只解首帧、返回「有进度」但内容每轮完全相同，
    // 通用路径又一次性解完整段。结果是 `while (ret != EOS)` 在 FLAC/OGG 上永不终止。
    // 要让它真流式，必须让解码器实例跨调用常驻，并新增「线程运行期间禁止调用」的约束——
    // 为一个零生产调用方的接口引入第三套生命周期并不划算。需要同步整段解码时用
    // AudioDecodeProcess（整段解成 AudioBuffer），需要流式时用 start() + 消费 ring。

protected:
    void onAudioDecodeCallback(AudioDecodeSpec &out) override;

private:
    size_t calculateMinWriteSpace(AudioCodecID codecId) const;

    // runDecode() 一次性解完整个 payload，成功即「流已耗尽」，不存在「还有数据」的中间态。
    // 用枚举而不是 0/-1，避免再把「成功」映射成「还有进度」而看不出矛盾。
    enum class RunDecodeResult { Completed, Error, Abort };

    void threadFunc();
    RunDecodeResult runDecode();
    bool writeDecodedBytes(const char *pcm, size_t pcmSize);
    // EOS 汇总：比「解出多少」与「标称多长」，短于 5pp 就报 DataTruncated（规格 T4 / 条目 23）。
    // 唯一出口：调用后状态要么 EOS，要么 ERROR。
    void finishAtEndOfStream();
    // 唯一的 ERROR 出口：先记原因，再置状态（状态是发布信号）。
    void setError(sdk::ErrorCode code, const char *detail);

private:
    AudioRingBuffer *m_ring;
    AudioSpec m_devSpec;
    AudioSpec m_srcSpec;
    ExtractorHelper *m_extractor;
    AudioCodecID m_codecID;
    uint64_t m_durationMs;

    Optional<Lazy<std::shared_ptr<AudioResample>>> m_lazyResample;
    std::vector<char> m_interleaveBuf;
    std::vector<char> m_resampleBuf;

    std::atomic<StreamDecoderState> m_state;
    std::atomic<sdk::ErrorCode> m_errorCode;
    mutable std::mutex m_errorMutex;
    std::string m_errorDetail;
    std::atomic<bool> m_stopRequest;
    std::atomic<int64_t> m_seekRequestMs;
    size_t m_seekDiscardBytes;
    size_t m_processedBytes;
    std::atomic<uint64_t> m_consumedBytes;
    uint64_t m_bytesPerMs;
    std::atomic<bool> m_abortDecode;

    // EOS 汇总的计量：解码产出的**源格式**帧数与采样率。
    // 刻意复用 positionMs()/m_consumedBytes 之外的计数器——那两处只在 seek 路径被赋值（#34 findings 1）。
    uint64_t m_decodedFrames;
    uint32_t m_decodedRate;
    size_t m_pcmFrameBytes;
    // seek 之后 measured 从 seek 目标算起，否则会被判成「短于标称」
    uint64_t m_seekBaseMs;

    std::thread m_thread;
};
