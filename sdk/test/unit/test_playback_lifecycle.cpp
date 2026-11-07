#include "../fixtures/RealMediaFixture.hpp"
#include <AudioBuffer.h>
#include <AudioCommon.hpp>
#include <AudioRingBuffer.h>
#include <AudioStreamDecoder.h>
#include <ExtractorHelper.hpp>
#include <MusicPlayList.h>
#include <doctest/doctest.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <thread>
#include <vector>

// 播放任务所有权与生命周期（#59 / 规格 #55 第 4 节）。
//
// 两个接缝，都是既有测试已经在用的公开边界：
//   1. AudioStreamDecoder::start() → 消费端从 ring 取走 PCM（与坏输入 / 预算 / 位置套件一致）；
//   2. MusicPlayList 的同步控制入口（addMusicWithNotify / startCurrentTask / stopCurrentTask /
//      currentTask / setCurrentIndexSync），不起设备、不进 10 ms push 循环。
//
// 并发类断言一律用「计数 / 门闩 / 超时上界」，不靠 sleep 猜时序：
//   - 门闩：等某个可观察状态出现，超时即判失败，不假设它一定在固定时刻出现；
//   - 超时上界：必须在有界窗口内观察到（或观察不到）某个量。
namespace {

constexpr int kSampleRate      = 48000;
constexpr int kChannels        = 1;
constexpr int kBytesPerSample  = 2;
constexpr uint64_t kBytesPerMs = (uint64_t)kSampleRate * kChannels * kBytesPerSample / 1000;

AudioSpec makeSpec(uint64_t durationMs)
{
    AudioSpec spec;
    spec.sampleRate     = kSampleRate;
    spec.numChannel     = kChannels;
    spec.bitsPerSample  = 16;
    spec.bytesPerSample = kBytesPerSample;
    spec.format         = AudioFormatS16;
    spec.samples        = 0;
    spec.durationMs     = durationMs;
    return spec;
}

// PCM 内容带位置指纹：每 1024 字节抬一格。用来判断「消费端拿到的到底是哪一段」——
// 只有内容本身能区分「seek 到 900 ms」与「seek 到 1900 ms」，位置计数器分不出来。
char patternByteAt(size_t offset)
{
    return static_cast<char>((offset / 1024) & 0xFF);
}

// 整段 PCM 的假提取器：AUDIO_CODEC_ID_NONE 走逐包供给分支（基类把整块 PCM 当作一包），
// 没有真实解码器依赖，时钟与内容都确定。
class PatternPcmExtractor : public ExtractorHelper {
public:
    PatternPcmExtractor(const AudioSpec &spec, size_t pcmBytes)
        : m_spec(spec)
        , m_pcm(std::make_shared<AudioBuffer>(pcmBytes))
    {
        for (size_t i = 0; i < pcmBytes; ++i) {
            m_pcm->data()[i] = patternByteAt(i);
        }
    }

    sdk_utils::status_t initCheck() override { return sdk_utils::OK; }
    AudioSpec getAudioSpec() override { return m_spec; }
    AudioCodecID getAudioCodecID() override { return AUDIO_CODEC_ID_NONE; }
    AudioBuffer::AudioBufferPtr getMetaData() override { return m_pcm; }
    DataSourceBase *getDataSource() override { return nullptr; }
    off64_t getDataSize() override { return static_cast<off64_t>(m_pcm->size()); }
    off64_t getAudioDataOffset() override { return 0; }

private:
    AudioSpec m_spec;
    AudioBuffer::AudioBufferPtr m_pcm;
};

// 门闩：等 predicate 成立，超时返回 false。轮询间隔 1 ms，超时上界由调用方给。
bool waitUntil(const std::function<bool()> &predicate, int timeoutMs)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

// 消费端驱动：从 ring 取走恰好 bytes 字节（跨多次 read），超时返回已取到的量。
size_t drainBytes(AudioRingBuffer &ring, size_t bytes, int timeoutMs = 5000)
{
    std::vector<char> buf(4096);
    size_t total = 0;
    int waited   = 0;
    while (total < bytes && waited <= timeoutMs) {
        const size_t avail = ring.availableRead();
        if (avail == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            waited += 2;
            continue;
        }
        const size_t once = std::min({ buf.size(), avail, bytes - total });
        const size_t got  = ring.read(buf.data(), once);
        if (got == 0) {
            break;
        }
        total += got;
    }
    return total;
}

// 播放列表侧的接缝：不接设备、不进 10 ms push，只看任务所有权。
struct RecordingPlayListCallback : public sdk::MusicPlayListCallback {
    int currentBufCount  = 0;
    int lastCurrentIndex = -1;

    void putMusicPlayListCurBuf(sdk::MusicPropertiesPtr property) override
    {
        ++currentBufCount;
        lastCurrentIndex = property ? static_cast<int>(property->index) : -1;
    }
    void updateMusicList(std::vector<sdk::MusicPropertiesPtr> &) override { }
    void onMusicPlayListError(sdk::ErrorCode, const std::string &, int,
                              const std::string &) override
    {
    }
};

// 设备规格 = 源规格（48 kHz 单声道 S16）：不触发重采样，ring 里的字节可以直接对源偏移。
AudioSpec makeDeviceSpec()
{
    AudioSpec spec = makeSpec(0);
    return spec;
}

std::shared_ptr<sdk::MusicPlayList> makePlayList(RecordingPlayListCallback &callback,
                                                 WorkQueue &workQueue,
                                                 const std::vector<std::string> &paths)
{
    AudioSpec devSpec = makeDeviceSpec();
    std::shared_ptr<sdk::MusicPlayList> playlist
        = std::make_shared<sdk::MusicPlayList>(&callback, &workQueue, devSpec);
    for (size_t i = 0; i < paths.size(); ++i) {
        std::promise<void> done;
        playlist->addMusicWithNotify(paths[i], [&done]() { done.set_value(); });
        done.get_future().wait();
    }
    return playlist;
}

} // namespace

TEST_SUITE("PlaybackLifecycle")
{
    TEST_CASE("the last of two back-to-back seeks wins")
    {
        AudioRingBuffer ring(AudioRingBuffer::kDefaultCapacity);
        AudioSpec spec = makeSpec(30000);
        // 30 s 的整段 PCM 远大于 ring：第一次 seek 之后解码线程会在写满 ring 时停住，
        // 不会整段解完，所以「解码线程此刻正处在 seek 静默窗口」是可观察、可等待的门闩。
        PatternPcmExtractor extractor(spec, static_cast<size_t>(30000 * kBytesPerMs));
        AudioStreamDecoder decoder(&ring, spec);

        REQUIRE(decoder.start(&extractor) == sdk_utils::OK);

        decoder.seekToMs(900);
        // 等到第一次 seek 被解码线程取走，判据给两个：窗口出现，或者它已经落地。
        //
        // 只等 SEEKING 是错的：这个窗口只有 12 ms——解码线程进入后 sleep_for(12ms) 才去
        // 清 ring；而 waitUntil 的 1 ms 采样在 Windows 上会退到 15.6 ms 计时器粒度，
        // 高负载下主线程调度还会更晚，窗口可以整段落在两次采样之间。那时再等 5 s 也等
        // 不到（CI 与本机全量并行时都红过）。「位置变成 900」是同一件事的可观察结果，
        // 且不受采样粒度影响，两种情形都说明第一次 seek 已被取走、可以发第二次了。
        REQUIRE_MESSAGE(waitUntil([&decoder]() {
                            return decoder.state() == StreamDecoderState::SEEKING
                                || decoder.positionMs() == 900;
                        }, 5000),
                        "第一次 seek 既没进 SEEKING 窗口也没落地，位置停在 "
                            << decoder.positionMs() << " ms");
        decoder.seekToMs(1900);

        // 后一次 seek 必须覆盖前一次：段基准落在 1900 ms。
        REQUIRE_MESSAGE(waitUntil([&decoder]() { return decoder.positionMs() == 1900; }, 5000),
                        "后一次 seek 没有生效，位置停在 " << decoder.positionMs() << " ms");

        // 内容才是判据：reset() 之后交付的第一个字节必须来自 1900 ms 处的源 PCM。
        // 若前一次 seek 活了下来，这里会读到 900 ms 的内容。
        const size_t expectedOffset = static_cast<size_t>(1900 * kBytesPerMs);
        char head[16]               = { 0 };
        REQUIRE_MESSAGE(waitUntil([&ring]() { return ring.availableRead() > 0; }, 5000),
                        "seek 之后 ring 里没有任何 PCM");
        REQUIRE(ring.read(head, sizeof(head)) == sizeof(head));
        CHECK_MESSAGE(head[0] == patternByteAt(expectedOffset),
                      "seek 后交付的 PCM 不是最后一次 seek 的目标段：期望源偏移 " << expectedOffset
                      << "，实得 " << static_cast<int>(static_cast<unsigned char>(head[0]))
                      << "，期望 " << static_cast<int>(
                             static_cast<unsigned char>(patternByteAt(expectedOffset))));

        decoder.stop();
    }

    TEST_CASE("switching tracks releases the previous task instead of reusing it")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music.wav")) {
            return;
        }

        RecordingPlayListCallback callback;
        WorkQueue workQueue;
        const std::string path = fixture.mediaPath("music.wav");
        std::shared_ptr<sdk::MusicPlayList> playlist
            = makePlayList(callback, workQueue, { path, path });
        REQUIRE(playlist->getMusicCount() == 2);

        // 扫描只留元数据：进列表不等于进播放任务。
        CHECK_MESSAGE(playlist->currentTask() == nullptr, "扫描阶段就建了播放任务（预缓冲持有 ring）");

        REQUIRE(playlist->startCurrentTask());
        sdk::PlaybackTaskPtr first = playlist->currentTask();
        REQUIRE(first != nullptr);
        CHECK(first->trackIndex == 0);
        REQUIRE_MESSAGE(waitUntil([&first]() { return first->ring->availableRead() > 0; }, 5000),
                        "任务启动后 ring 里没有 PCM");

        std::weak_ptr<AudioRingBuffer> weakOldRing       = first->ring;
        std::weak_ptr<AudioStreamDecoder> weakOldDecoder = first->decoder;
        AudioRingBuffer *oldRingAddress                  = first->ring.get();
        first.reset(); // 测试不再持有旧任务

        // 切歌 = 换一个播放任务：新 ring、新 decoder、位置从 0 起算。
        REQUIRE(playlist->setCurrentIndexSync(1));
        sdk::PlaybackTaskPtr second = playlist->currentTask();
        REQUIRE(second != nullptr);
        CHECK(second->trackIndex == 1);
        CHECK_MESSAGE(second->ring.get() != oldRingAddress, "切歌复用了上一首的 ring 实例");
        CHECK_MESSAGE(second->ring->readPos() == 0, "新任务的 ring 段内读计数没有归零");
        CHECK_MESSAGE(second->decoder->positionMs() == 0, "新任务的位置没有从 0 起算");

        // 旧任务必须在新任务启动前就被销毁：ring 不可复用，decode 线程也必须已经 join
        // （否则销毁 ring 就是让后台线程写已释放内存，这条断言会以崩溃或写穿的形式失败）。
        CHECK_MESSAGE(weakOldRing.expired(), "旧任务的 ring 没有在切歌时释放");
        CHECK_MESSAGE(weakOldDecoder.expired(), "旧任务的 decoder 没有在切歌时销毁（join 未完成）");

        playlist->stopCurrentTask();
    }

    TEST_CASE("stop drops the task and the next play starts a fresh one")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music.wav")) {
            return;
        }

        RecordingPlayListCallback callback;
        WorkQueue workQueue;
        const std::string path = fixture.mediaPath("music.wav");
        std::shared_ptr<sdk::MusicPlayList> playlist
            = makePlayList(callback, workQueue, { path });

        REQUIRE(playlist->startCurrentTask());
        sdk::PlaybackTaskPtr task = playlist->currentTask();
        REQUIRE(task != nullptr);
        // stop 之前 ring 里确实还有没播完的 PCM：这样「stop 不 drain」才是可观察的，
        // 而不是碰巧 ring 本来就是空的。
        REQUIRE_MESSAGE(waitUntil([&task]() { return task->ring->availableRead() > 0; }, 5000),
                        "任务启动后 ring 里没有 PCM");

        std::weak_ptr<AudioRingBuffer> weakRing          = task->ring;
        std::weak_ptr<AudioStreamDecoder> weakDecoder    = task->decoder;
        AudioRingBuffer *previousRingAddress             = task->ring.get();
        task.reset();

        playlist->stopCurrentTask();

        // stop 之后没有任务 = 消费端没有 ring 可读，只能出静音；旧 PCM 随 ring 一起丢弃。
        CHECK_MESSAGE(playlist->currentTask() == nullptr, "stop 之后还挂着播放任务");
        CHECK_MESSAGE(weakRing.expired(), "stop 之后旧 ring 没有被释放（旧 PCM 还留着）");
        CHECK_MESSAGE(weakDecoder.expired(), "stop 之后解码线程没有被 join 并销毁");

        // 幂等：重复 stop 不改变结论。
        playlist->stopCurrentTask();
        CHECK(playlist->currentTask() == nullptr);

        // stop 期间到达的 play 排到 stop 完成之后执行：它看到的是干净的任务，
        // 新 ring、位置从 0 起算，不会接着上一首的缓冲播。
        REQUIRE(playlist->startCurrentTask());
        sdk::PlaybackTaskPtr replay = playlist->currentTask();
        REQUIRE(replay != nullptr);
        CHECK_MESSAGE(replay->ring.get() != previousRingAddress, "stop 之后 play 复用了旧的 ring");
        CHECK_MESSAGE(replay->decoder->positionMs() == 0, "stop 之后 play 没有从 0 起算");

        playlist->stopCurrentTask();
    }

    TEST_CASE("seek issued while no task runs is dropped, not deferred to the next task")
    {
        AudioSpec spec = makeSpec(3000);
        PatternPcmExtractor extractor(spec, static_cast<size_t>(3000 * kBytesPerMs));

        AudioRingBuffer ring(AudioRingBuffer::kDefaultCapacity);
        AudioStreamDecoder decoder(&ring, spec);
        REQUIRE(decoder.start(&extractor) == sdk_utils::OK);
        REQUIRE(drainBytes(ring, static_cast<size_t>(25 * kBytesPerMs)) > 0);
        const uint64_t positionBeforeStop = decoder.positionMs();

        decoder.stop();
        REQUIRE(decoder.state() == StreamDecoderState::IDLE);

        // 停机窗口 = 切歌 / 停止中：这一次 seek 必须当场丢掉。
        decoder.seekToMs(1000);
        CHECK_MESSAGE(decoder.state() == StreamDecoderState::IDLE, "停机期间的 seek 把解码器拉进了 SEEKING");
        CHECK_MESSAGE(decoder.positionMs() == positionBeforeStop,
                      "停机期间的 seek 改写了位置：" << decoder.positionMs() << " ms");

        // 下一个任务从自己的段起算：丢弃的 seek 不许迟到生效。
        AudioRingBuffer nextRing(AudioRingBuffer::kDefaultCapacity);
        AudioStreamDecoder nextDecoder(&nextRing, spec);
        REQUIRE(nextDecoder.start(&extractor) == sdk_utils::OK);
        CHECK_MESSAGE(nextDecoder.positionMs() == 0, "新任务的段基准被停机窗口里的 seek 带偏了");
        REQUIRE(drainBytes(nextRing, static_cast<size_t>(25 * kBytesPerMs)) > 0);
        CHECK_MESSAGE(nextDecoder.positionMs() == 25,
                      "新任务的位置被停机窗口里的 seek 带偏到 " << nextDecoder.positionMs() << " ms");
        nextDecoder.stop();
    }
}
