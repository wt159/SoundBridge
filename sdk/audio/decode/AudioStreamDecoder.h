#pragma once

#include "../../ErrorCode.hpp"
#include "../resample/AudioResample.h"
#include "AudioCommon.hpp"
#include "AudioDecode.h"
#include "AudioRingBuffer.h"
#include "DecodeSupplyObserver.h"
#include "ErrorUtils.h"
#include "ExtractorHelper.hpp"
#include "Lazy.hpp"
#include "NonCopyable.hpp"
#include "Optional.hpp"
#include "StreamMetrics.h"
#include <atomic>
#include <chrono>
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
class AudioDecode;

enum class StreamDecoderState { IDLE, DECODING, SEEKING, EOS, ERROR };

class AudioStreamDecoder : public NonCopyable,
                            public AudioDecodeCallback,
                            public DecodeSupplyObserver {
public:
    AudioStreamDecoder(AudioRingBuffer *ring, const AudioSpec &devSpec);
    ~AudioStreamDecoder();

    // 启动一次播放任务：先 stop() + join 上一次任务，再起一个新的解码线程。
    // 每次 start() 必定是新线程；join 由调用线程在起线程之前完成，禁止 detach / 后台 join。
    // 新任务自带新的 ring（规格 4.3），所以这里不清 ring：reset() 由新解码线程的第一件事做。
    sdk_utils::status_t start(ExtractorHelper *extractor);
    // startThread=false 只做初始化（供只检查状态/生命周期的场合），不会产出任何数据：
    // 同步解码入口已按 #18 删除，解码只能由 threadFunc 驱动。
    sdk_utils::status_t start(ExtractorHelper *extractor, bool startThread);
    // 置取消位并 join。幂等；join 只在调用它的线程上发生。
    void stop();
    void seekToMs(uint64_t targetMs);

    StreamDecoderState state() const;
    // 播放位置 = (segmentBaseBytes + ring 段内已读字节) / bytesPerMs（规格 #55 第 3 节）。
    // 段基准在 seek / 切歌（start）时更新；durationMs 为 0（未知）时位置照常按已消费 PCM 报，
    // 不凭空推算总长、也不把位置夹到 [0, duration] 之外。
    uint64_t positionMs() const;
    // 提取器 init 期给出的标称时长，播放期间不变；0 表示未知。
    uint64_t durationMs() const;
    bool canDecode() const;

    // 进入 ERROR 之前先写原因：错误码只表达原因，处置由调用方按 ErrorInfo 决定
    // （docs/adr/0001-error-code-expresses-cause.md）。未进入 ERROR 时返回 Ok。
    sdk::ErrorCode errorCode() const;
    std::string errorDetail() const;

    // 四个观测点的快照（规格第 5 节 / 票面 #56）。它们是仪表，不是判据：
    // 本票只要求可读、可复现，不在这里对任何数值判红绿。
    // 线程安全：可从消费线程读取，不影响解码线程。
    StreamMetrics metrics() const;
    // 消费端在静默窗口交付一块静音时调用，供 metrics().silenceFills 记账。
    // 生产消费端（MusicPlayer::Impl::consumeAudioData）与判据套件的消费端走同一个入口。
    // 只有解码器自己看得见的静默窗口（SEEKING / 已请求停止 / 已置取消位）里的调用才计数。
    void noteSilenceFill();

    // 刻意不提供同步的 decodeNext()（#18）。它曾对三类 codec 有三套语义：
    // FLAC/Vorbis 每次调用新建解码器 → 只解首帧、返回「有进度」但内容每轮完全相同，
    // 通用路径又一次性解完整段。结果是 `while (ret != EOS)` 在 FLAC/OGG 上永不终止。
    // 要让它真流式，必须让解码器实例跨调用常驻，并新增「线程运行期间禁止调用」的约束——
    // 为一个零生产调用方的接口引入第三套生命周期并不划算。需要同步整段解码时用
    // AudioDecodeProcess（整段解成 AudioBuffer），需要流式时用 start() + 消费 ring。

protected:
    void onAudioDecodeCallback(AudioDecodeSpec &out) override;
    // 单包解不出、已被 ffmpeg 路径跳过：只记账，EOS 时汇总成一条 DataCorrupted。
    void onAudioDecodeSkip() override;

public:
    // 增量供给的记账钩子（DecodeSupplyObserver，票面 #60）：由解码线程在拉包/放包时调用。
    // 持有量会先增后减，峰值必须挂在 alloc/free 事件上重新采样，不能只在 start/首帧/EOS 采。
    void onPacketAcquired(size_t bytes) override;
    void onPacketReleased(size_t bytes) override;
    void onPacketPulled(size_t totalPulled) override;

private:
    size_t calculateMinWriteSpace(AudioCodecID codecId) const;
    // 与 ring 内容同一档的 bytesPerMs：重采样生效时是设备格式，否则是源格式。
    // 段基准与 ring 段内计数都以这一档字节记账，不能混用 seek 丢弃用的源字节口径。
    uint64_t positionBytesPerMs() const;
    // 把 seek 目标写成段基准；字节口径未知（首帧之前）时寄存目标，首帧后补算。
    void applySegmentBase(uint64_t targetMs);
    // 取消请求是否仍然有效：停止 / 取消位 / 槽里还挂着没被处理的 seek 目标。
    // 解码线程只把取消当作**结果**，从不把它当成取消信号读回来。
    bool cancelRequested() const;

    void threadFunc();
    // 增量供给的 ffmpeg 分支（#62 / ADR 0003）：mp3 / aac 逐包喂、整个任务只建一次
    // 编解码器；seek 按「顺序重放」重建解码器并从包序列起点重新迭代（#49 第二档）。
    std::unique_ptr<AudioDecode> createFfmpegPacketDecoder();
    void runFfmpegPacketLoop();
    bool writeDecodedBytes(const char *pcm, size_t pcmSize);
    // 按当前的 m_srcSpec / m_devSpec 造一个重采样器；失败返回 nullptr。
    std::shared_ptr<AudioResample> createResampler();
    // 设备规格是否带 PCM 格式。缺了它就没有重采样的目标格式（单测会拿提取器 spec
    // 当设备规格，此时 format 是 Unknown），这种情况下退化成不重采样。
    bool deviceSpecSupportsResample() const;
    // EOS 汇总：比「解出多少」与「标称多长」，短于 5pp 就报 DataTruncated（规格 T4 / 条目 23）。
    // 唯一出口：调用后状态要么 EOS，要么 ERROR。
    void finishAtEndOfStream();
    // 记一次「这一帧/这一包解不出，跳过继续」（规格 4.1 / F1）。
    // 不立即置 ERROR：坏帧的处置是播到结尾再汇总一条 DataCorrupted。
    void noteSkippedFrame(uint64_t count = 1);
    // 分配记账：把「此刻为当前曲目持有多少字节」喂给峰值。
    // 增量供给下持有量会先增后减，所以调用点挂在 alloc/free 事件上（见 refreshHeldPeak）。
    void noteHeldBytes(size_t bytes);
    // 重新算一次「当前持有量」并喂给峰值：解码侧手上的包 + 提取器的解复用缓冲 + 解码器
    // 自己那两块固定缓冲（O(1)）。ring 与 AVIO 是判据公式里的独立项，不在这里计。
    void refreshHeldPeak();
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
    // 首帧的实测规格还没覆盖过 m_srcSpec（规格 6.2 S3：无条件覆盖一次）。
    bool m_firstDecodeFrame;
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
    // 段基准（消费端 O(1)，不是 ring 成员）：seek / 切歌时更新。
    // ring 的 reset() 会把段内读计数归零，所以基准必须存在 ring 之外，
    // 否则位置会随 seek 跳回 0。单位与 ring 内容一致（见 positionBytesPerMs）。
    std::atomic<uint64_t> m_segmentBaseBytes;
    // 首帧之前还不知道 PCM 格式（mp3 / aac 等容器头不声明 bytesPerSample），
    // 此时算不出段基准的字节数。先把 seek 目标寄存在这里，首帧确定字节口径后补算。
    uint64_t m_pendingSeekBaseMs;
    uint64_t m_bytesPerMs;
    std::atomic<bool> m_abortDecode;

    // EOS 汇总的计量：解码产出的**源格式**帧数与采样率。
    // 刻意不复用 positionMs() 背后的段基准 / ring 读计数——那两处只回答「消费端走到哪」，
    // 而 EOS 比的是「解码器产出多少」（#34 findings 1）。
    uint64_t m_decodedFrames;
    uint32_t m_decodedRate;
    size_t m_pcmFrameBytes;
    // seek 之后 measured 从 seek 目标算起，否则会被判成「短于标称」
    uint64_t m_seekBaseMs;

    // 坏帧（规格 4.1 / F1）：跳过的帧/页/包数只进日志与 detail，不做机器可读字段。
    uint64_t m_skippedFrames;
    // 有一次跳帧还没遇到「后续成功解出」——失败若发生在数据末尾（后面没有数据了），
    // 那是截断而不是坏帧（4.1 的判别原则）。
    bool m_pendingFrameSkip;
    // 跳帧之后确实还有数据 → 坏帧。F2：与「短于标称」同时成立时报 DataCorrupted。
    bool m_frameSkipWithDataAfter;
    // flac 的 STREAMINFO MD5 不符（条目 10）。截断同样会让 MD5 不符，所以只在
    // 「解到标称长度」时才当作坏帧，解不到长度的交给 DataTruncated（4.1）。
    bool m_md5Mismatch;

    // 四个观测点：全部是「本次播放任务」的口径，start() 清零后重新累积。
    // 消费线程会写 silenceFills，所以用原子量；其余只在解码线程写、任意线程读。
    std::atomic<size_t> m_peakHeldBytes;
    std::atomic<size_t> m_silenceFills;
    std::atomic<uint64_t> m_startToFirstPcmUs;
    std::atomic<uint64_t> m_initScanUs;
    // 解码侧此刻手上的包字节数（alloc/free 增量，只在解码线程读写）。
    size_t m_packetHeldBytes;
    // 本任务已拉到的包数，与「首帧那一刻」的快照（并行判据的结构证据）。
    std::atomic<size_t> m_packetsPulled;
    std::atomic<size_t> m_packetsPulledAtFirstPcm;
    std::chrono::steady_clock::time_point m_startClock;
    bool m_firstPcmNoted;

    std::thread m_thread;
};
