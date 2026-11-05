#include <AudioBuffer.h>
#include <AudioCommon.hpp>
#include <AudioRingBuffer.h>
#include <AudioStreamDecoder.h>
#include <ExtractorHelper.hpp>
#include <doctest/doctest.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

// 播放位置单轨化（#58 / 规格 #55 第 3、4 节）。
//
// 接缝与坏输入 / 预算套件一致：AudioStreamDecoder::start() → 消费端从 ring 取走 PCM。
// 这里不碰 MusicPlayer 的 10 ms 定时 push，也不引入 mock 设备：
//   positionMs = (segmentBaseBytes + ring 段内已读字节) / bytesPerMs
// 唯一可观察的量就是「消费端取走了多少字节 → 报了多少毫秒」。
//
// 素材用合成 PCM（AUDIO_CODEC_ID_NONE 直接吐整段），选 48 kHz 单声道 S16：
// bytesPerMs = 48000 * 1 * 2 / 1000 = 96，整除无截断，断言可以精确到毫秒。
// 2400 字节正好 25 ms，作为消费步长。
namespace {

constexpr int kSampleRate     = 48000;
constexpr int kChannels       = 1;
constexpr int kBytesPerSample = 2;
constexpr size_t kChunkBytes  = 2400; // 25 ms
constexpr uint64_t kBytesPerMs = 96;

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

size_t pcmBytesForMs(uint64_t ms)
{
    return static_cast<size_t>(ms * kBytesPerMs);
}

// 整段 PCM 的假提取器：AUDIO_CODEC_ID_NONE 走 runDecode 的「一次性吐 getMetaData」分支，
// 没有真实解码器依赖，行为确定。
class PcmExtractor : public ExtractorHelper {
public:
    PcmExtractor(const AudioSpec &spec, size_t pcmBytes)
        : m_spec(spec)
        , m_pcm(std::make_shared<AudioBuffer>(pcmBytes))
    {
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

} // namespace

TEST_SUITE("PlaybackPosition")
{
    TEST_CASE("position advances with consumed pcm and never rewinds")
    {
        AudioRingBuffer ring(AudioRingBuffer::kDefaultCapacity);
        AudioSpec spec = makeSpec(2000);
        PcmExtractor extractor(spec, pcmBytesForMs(2000));
        AudioStreamDecoder decoder(&ring, spec);

        REQUIRE(decoder.start(&extractor) == sdk_utils::OK);
        CHECK_MESSAGE(decoder.positionMs() == 0, "新曲目的位置必须从 0 起算");

        // 稳态播放：消费端每取走 25 ms，位置就必须前进 25 ms，且不回头。
        uint64_t lastPosition = 0;
        for (uint64_t step = 1; step <= 8; ++step) {
            REQUIRE(drainBytes(ring, kChunkBytes) == kChunkBytes);
            const uint64_t position = decoder.positionMs();
            CHECK_MESSAGE(position == step * 25,
                          "已消费 " << step * kChunkBytes << " 字节，位置应为 " << step * 25
                                    << " ms，实际 " << position << " ms");
            CHECK_MESSAGE(position >= lastPosition, "位置回退：" << lastPosition << " -> " << position);
            lastPosition = position;
        }

        decoder.stop();
    }

    TEST_CASE("seek lands on the target and ring reset keeps the segment base")
    {
        AudioRingBuffer ring(AudioRingBuffer::kDefaultCapacity);
        AudioSpec spec = makeSpec(3000);
        PcmExtractor extractor(spec, pcmBytesForMs(3000));
        AudioStreamDecoder decoder(&ring, spec);

        REQUIRE(decoder.start(&extractor) == sdk_utils::OK);
        REQUIRE(drainBytes(ring, pcmBytesForMs(500)) == pcmBytesForMs(500));
        REQUIRE(decoder.positionMs() == 500);

        decoder.seekToMs(2000);

        // seek 期间 ring 会被 reset（段内已读字节归零）。段基准必须接住这段位移，
        // 因此位置只能从 500 跳到 2000，绝不允许先掉回 0。
        int waited      = 0;
        uint64_t lowest = decoder.positionMs();
        while (waited <= 5000) {
            const StreamDecoderState state = decoder.state();
            const uint64_t position        = decoder.positionMs();
            lowest                         = std::min(lowest, position);
            CHECK_MESSAGE(position >= 500, "seek 途中位置掉回了 " << position << " ms");
            if (position == 2000) {
                break; // 目标已落地；此时 EOS 与 DECODING 都可能（剩余 PCM 已写进 ring）
            }
            if (state == StreamDecoderState::ERROR) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            waited += 2;
        }

        CHECK(decoder.state() != StreamDecoderState::ERROR);
        CHECK_MESSAGE(lowest >= 500, "段基准没有接住 ring 归零，最低读到 " << lowest << " ms");
        REQUIRE(decoder.positionMs() == 2000);

        // 从目标点继续消费，位置从 2000 往前推。
        REQUIRE(drainBytes(ring, kChunkBytes) == kChunkBytes);
        CHECK(decoder.positionMs() == 2025);

        decoder.stop();
    }

    TEST_CASE("starting a new track restarts the segment from zero")
    {
        AudioRingBuffer ring(AudioRingBuffer::kDefaultCapacity);
        AudioSpec spec = makeSpec(1000);
        PcmExtractor first(spec, pcmBytesForMs(1000));
        PcmExtractor second(spec, pcmBytesForMs(1000));
        AudioStreamDecoder decoder(&ring, spec);

        REQUIRE(decoder.start(&first) == sdk_utils::OK);
        REQUIRE(drainBytes(ring, kChunkBytes) == kChunkBytes);
        REQUIRE(decoder.positionMs() == 25);

        // 切歌 = 新任务 start()：新段基准必须归零，旧曲的已消费字节不许带过来。
        REQUIRE(decoder.start(&second) == sdk_utils::OK);
        CHECK(decoder.positionMs() == 0);

        REQUIRE(drainBytes(ring, kChunkBytes) == kChunkBytes);
        CHECK(decoder.positionMs() == 25);

        decoder.stop();
    }

    TEST_CASE("unknown duration keeps position on the consumed pcm track")
    {
        AudioRingBuffer ring(AudioRingBuffer::kDefaultCapacity);
        AudioSpec spec = makeSpec(0); // 0 = 未知
        PcmExtractor extractor(spec, pcmBytesForMs(1000));
        AudioStreamDecoder decoder(&ring, spec);

        REQUIRE(decoder.start(&extractor) == sdk_utils::OK);
        CHECK_MESSAGE(decoder.durationMs() == 0, "未知时长必须如实报 0，不许凭空推算");

        REQUIRE(drainBytes(ring, kChunkBytes) == kChunkBytes);
        CHECK_MESSAGE(decoder.positionMs() == 25,
                      "时长未知不影响位置：位置只由已消费 PCM 决定");

        decoder.stop();
    }

    TEST_CASE("duration comes from the extractor and stays constant while playing")
    {
        AudioRingBuffer ring(AudioRingBuffer::kDefaultCapacity);
        AudioSpec spec = makeSpec(12345);
        PcmExtractor extractor(spec, pcmBytesForMs(500));
        AudioStreamDecoder decoder(&ring, spec);

        REQUIRE(decoder.start(&extractor) == sdk_utils::OK);
        CHECK(decoder.durationMs() == 12345);

        REQUIRE(drainBytes(ring, kChunkBytes) == kChunkBytes);
        CHECK_MESSAGE(decoder.durationMs() == 12345, "播放期间时长必须保持不变");

        decoder.stop();
    }
}
