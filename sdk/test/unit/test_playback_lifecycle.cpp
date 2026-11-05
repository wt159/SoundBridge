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

// 整段 PCM 的假提取器：AUDIO_CODEC_ID_NONE 走 runDecode 的一次性交付分支，
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
        // 解码线程把状态置成 SEEKING 之后才开始清 ring；等到它即说明第一次 seek 已被取走。
        REQUIRE_MESSAGE(waitUntil([&decoder]() {
                            return decoder.state() == StreamDecoderState::SEEKING;
                        }, 5000),
                        "解码线程没有进入 SEEKING");
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
}
