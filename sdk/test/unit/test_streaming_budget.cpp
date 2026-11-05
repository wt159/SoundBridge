#include "../fixtures/RealMediaFixture.hpp"
#include <AudioCommon.hpp>
#include <AudioRingBuffer.h>
#include <AudioStreamDecoder.h>
#include <ExtractorFactory.h>
#include <FileSource.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <doctest/doctest.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// 边解边播的独立判据套件（规格 #55 第 5 节，票面 #56 / E5–E6）。
//
// 与 MediaCoverage（好文件 1% 容差）和 BadInput（坏文件 5pp 容差）三分：这里不放任何时长判据，
// 只记录四个观测点（分配记账峰值 / 稳态静音填充次数 / start→首帧时间戳 / init 只读扫描耗时）
// 在**当前链路**上的基线值。本票不判常驻上界的红绿，也不设阈值——迁移到增量供给
// （#60 起）之后，同一套观测点才会被常驻上界判据使用。
//
// 测试接缝与坏输入单测完全一致：AudioStreamDecoder::start() → 消费端从 ring 取走 PCM
// （见 test_bad_input.cpp:151 的 drainUntilSettled）。不引入 mock audio sink、
// 中间接口层或进程 RSS 观测。
TEST_SUITE("StreamingBudget")
{
    namespace
    {
        const char *kShortOgg = "Ringtones-耳聆网.ogg"; // 3.7 s
        const char *kLongWma  = "十七岁-陶喆.96.wma";    // 259 s，约 70 倍

        AudioSpec makeDevSpec()
        {
            AudioSpec spec;
            spec.sampleRate     = 44100;
            spec.numChannel     = 2;
            spec.bitsPerSample  = 16;
            spec.bytesPerSample = 2;
            spec.format         = AudioFormatS16;
            spec.samples        = 0;
            spec.durationMs     = 0;
            return spec;
        }

        // 消费端每次最多取走的字节数与轮询间隔。
        // 与生产链的 10 ms / 4096 B 节流不同：判据套件要把整首曲子跑完，
        // 按生产速率消费一首 259 s 素材需要 110 s 以上（规格第 5 节已注明
        // 「稳态欠流判据假定测试环境消费端不按实时墙钟节流」）。
        const size_t kChunkBytes = 4096;
        const int kPollMs        = 5;

        struct PlaybackObservation {
            bool opened                 = false;
            size_t bytes                = 0;
            StreamDecoderState endState = StreamDecoderState::IDLE;
            StreamMetrics metrics       = {};
        };

        // 静默窗口：生产消费端（MusicPlayer::Impl::shouldFillSilence）在其中写静音而不是读 ring。
        // 套件只负责认这个窗口，静音计数由解码器自己的仪表记账。
        bool isSilenceWindow(const AudioStreamDecoder &decoder)
        {
            return decoder.state() == StreamDecoderState::SEEKING;
        }

        PlaybackObservation playWholeTrack(const std::string &mediaDir, const std::string &fileName,
                                          const char *extension, int timeoutMs)
        {
            PlaybackObservation observation;
            const std::string path = mediaDir + "/" + fileName;

            FileSource source(path.c_str());
            if (source.initCheck() != sdk_utils::OK) {
                return observation;
            }
            // source 的生命周期必须覆盖整个解码过程：提取器只持它的裸指针。
            std::shared_ptr<ExtractorHelper> extractor(
                ExtractorFactory::createExtractor(&source, extension, true));
            if (extractor == nullptr || extractor->initCheck() != sdk_utils::OK) {
                return observation;
            }

            AudioRingBuffer ring(AudioRingBuffer::kDefaultCapacity);
            AudioStreamDecoder decoder(&ring, makeDevSpec());
            if (decoder.start(extractor.get()) != sdk_utils::OK) {
                observation.endState = decoder.state();
                observation.metrics  = decoder.metrics();
                return observation;
            }
            observation.opened = true;

            std::vector<char> buf(kChunkBytes);
            int elapsed = 0;
            while (elapsed <= timeoutMs) {
                if (isSilenceWindow(decoder)) {
                    // 静默窗口里消费端交付静音：交给解码器记账（与生产同一个计数入口）。
                    decoder.noteSilenceFill();
                } else {
                    while (ring.availableRead() > 0) {
                        size_t once = std::min(buf.size(), ring.availableRead());
                        size_t got  = ring.read(buf.data(), once);
                        if (got == 0) {
                            break;
                        }
                        observation.bytes += got;
                    }
                }

                StreamDecoderState state = decoder.state();
                if (state == StreamDecoderState::ERROR || state == StreamDecoderState::EOS) {
                    observation.endState = state;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
                elapsed += kPollMs;
            }

            observation.metrics = decoder.metrics();
            decoder.stop();
            return observation;
        }
    } // namespace

    TEST_CASE("instruments are readable on the playback path for short and long tracks")
    {
        RealMediaFixture fixture;
        if (!fixture.exists(kShortOgg) || !fixture.exists(kLongWma)) {
            std::printf("[SKIP] %s / %s (素材未随仓库下发)\n", kShortOgg, kLongWma);
            return;
        }

        const struct {
            const char *fileName;
            const char *extension;
            int timeoutMs;
        } tracks[] = {
            { kShortOgg, ".ogg", 60000 },  // 3.7 s
            { kLongWma, ".wma", 180000 },  // 259 s，约 70 倍
        };

        for (size_t i = 0; i < sizeof(tracks) / sizeof(tracks[0]); ++i) {
            INFO(std::string(tracks[i].fileName));
            PlaybackObservation observation
                = playWholeTrack(fixture.mediaDir(), tracks[i].fileName, tracks[i].extension,
                                 tracks[i].timeoutMs);
            REQUIRE(observation.opened);
            CHECK_MESSAGE(observation.bytes > 0, "消费端没有拿到任何 PCM");
            CHECK_MESSAGE(observation.metrics.startToFirstPcmUs > 0,
                          "start→首帧时间戳没有记上");
            CHECK_MESSAGE(observation.metrics.initScanUs > 0, "init 只读扫描耗时没有记上");
            CHECK_MESSAGE(observation.metrics.peakHeldBytes > 0, "分配记账峰值没有记上");
            MESSAGE(tracks[i].fileName
                    << ": initScanUs " << observation.metrics.initScanUs
                    << ", startToFirstPcmUs " << observation.metrics.startToFirstPcmUs
                    << ", peakHeldBytes " << observation.metrics.peakHeldBytes
                    << ", silenceFills " << observation.metrics.silenceFills
                    << ", deliveredBytes " << observation.bytes);
        }
    }
}
