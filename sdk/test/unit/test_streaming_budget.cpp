#include "../fixtures/RealMediaFixture.hpp"
#include <AudioCommon.hpp>
#include <AudioRingBuffer.h>
#include <AudioStreamDecoder.h>
#include <ExtractorFactory.h>
#include <ExtractorHelper.hpp>
#include <FileSource.h>
#include <algorithm>
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
            bool opened                  = false;
            size_t bytes                 = 0;
            StreamDecoderState endState  = StreamDecoderState::IDLE;
            sdk::ErrorCode error         = sdk::ErrorCode::Ok;
            std::string errorDetail;
            StreamMetrics metrics        = {};
        };
        // 静默窗口：生产消费端（MusicPlayer::Impl::shouldFillSilence）在其中写静音而不是读 ring。
        // 套件只负责认这个窗口，静音计数由解码器自己的仪表记账。
        bool isSilenceWindow(const AudioStreamDecoder &decoder)
        {
            return decoder.state() == StreamDecoderState::SEEKING;
        }

        // 一次播放的可选驱动参数。
        struct PlaybackPlan {
            // 消费端取走这么多字节后发起一次 seek；0 表示不 seek。
            // 用已交付字节而不是 positionMs()：positionMs() 只在 seek 路径上被赋值，
            // 稳态播放期间恒为 0（现状口径，见 AudioStreamDecoder::positionMs）。
            size_t seekAfterBytes   = 0;
            uint64_t seekTargetMs   = 0;
            // 在非静默窗口里故意记一次「静音填充」：用来锁住计数器只覆盖静默窗口。
            bool probeOutsideWindow = false;
        };

        // 把一次播放跑到底：解码器 + ring + 消费端，与生产链同一个接缝。
        // 提取器由调用方给出（生命周期由调用方保证），这样可以拿同一个解码器去跑
        // 两种供给形式（逐包 / 整块），按判据对照。
        PlaybackObservation playWithExtractor(ExtractorHelper *extractor, int timeoutMs,
                                              const PlaybackPlan &plan = PlaybackPlan())
        {
            PlaybackObservation observation;
            if (extractor == nullptr || extractor->initCheck() != sdk_utils::OK) {
                return observation;
            }

            AudioRingBuffer ring(AudioRingBuffer::kDefaultCapacity);
            AudioStreamDecoder decoder(&ring, makeDevSpec());
            if (decoder.start(extractor) != sdk_utils::OK) {
                observation.endState = decoder.state();
                observation.metrics  = decoder.metrics();
                observation.error    = decoder.errorCode();
                observation.errorDetail = decoder.errorDetail();
                return observation;
            }
            observation.opened = true;

            std::vector<char> buf(kChunkBytes);
            bool probed  = false;
            bool seeked  = false;
            int elapsed  = 0;
            while (elapsed <= timeoutMs) {
                if (isSilenceWindow(decoder)) {
                    // 静默窗口里消费端交付静音：交给解码器记账（与生产同一个计数入口）。
                    decoder.noteSilenceFill();
                } else {
                    if (plan.probeOutsideWindow && !probed) {
                        probed = true;
                        decoder.noteSilenceFill();
                    }
                    while (ring.availableRead() > 0) {
                        size_t once = std::min(buf.size(), ring.availableRead());
                        size_t got  = ring.read(buf.data(), once);
                        if (got == 0) {
                            break;
                        }
                        observation.bytes += got;
                    }
                }

                if (!seeked && plan.seekAfterBytes > 0
                    && observation.bytes >= plan.seekAfterBytes) {
                    seeked = true;
                    decoder.seekToMs(plan.seekTargetMs);
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
            observation.error   = decoder.errorCode();
            observation.errorDetail = decoder.errorDetail();
            if (observation.endState == StreamDecoderState::EOS) {
                // EOS 只是「解码侧不再产出」，ring 里可能还有没被消费掉的尾巴：
                // 判据要比「整首解出多少字节」，就把尾巴一起排掉。
                while (ring.availableRead() > 0) {
                    size_t once = std::min(buf.size(), ring.availableRead());
                    size_t got  = ring.read(buf.data(), once);
                    if (got == 0) {
                        break;
                    }
                    observation.bytes += got;
                }
            }
            decoder.stop();
            return observation;
        }

        PlaybackObservation playWholeTrack(const std::string &mediaDir, const std::string &fileName,
                                          const char *extension, int timeoutMs,
                                          const PlaybackPlan &plan = PlaybackPlan())
        {
            const std::string path = mediaDir + "/" + fileName;

            FileSource source(path.c_str());
            if (source.initCheck() != sdk_utils::OK) {
                return PlaybackObservation();
            }
            // source 的生命周期必须覆盖整个解码过程：提取器只持它的裸指针。
            std::shared_ptr<ExtractorHelper> extractor(
                ExtractorFactory::createExtractor(&source, extension, true));
            return playWithExtractor(extractor.get(), timeoutMs, plan);
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

    // —— 常驻上界与并行两条判据（规格第 5 节 / 票面 #60 的验收门禁）——
    //
    // 上界公式：262144(ring) + P + 64KB(AVIO) + O(1)，P = 该素材实测最大包 × 2。
    // heldPayloadBytes 的口径不含 ring 与 AVIO（它们是公式里的独立项），所以断言写成
    // 「peakHeldBytes ≤ 262144 + 2P + 65536」即可：左边不计 ring，右边给它留足。
    //
    // 并行判据不看墙钟：看首帧交付那一刻供给序列走到哪。整块供给（一次交出整首）在首帧时
    // 序列已经耗尽，两个数相等；逐包供给严格小于总数。这条断言是结构性的，不受机器负载影响。
    namespace
    {
        const char *kFlac = "小镇姑娘-陶喆.flac";
        const size_t kAvioHeadroomBytes = 64 * 1024;

        struct PacketProfile {
            size_t count          = 0;
            size_t maxPacketBytes = 0;
        };

        // 只量包形状，不解码：P 必须来自该素材的实测，不能写死常量。
        PacketProfile measurePacketProfile(const std::string &mediaDir, const char *name)
        {
            PacketProfile profile;
            FileSource source((mediaDir + "/" + name).c_str());
            if (source.initCheck() != sdk_utils::OK) {
                return profile;
            }
            std::shared_ptr<ExtractorHelper> extractor(
                ExtractorFactory::createExtractor(&source, ".flac", true));
            if (extractor == nullptr || extractor->initCheck() != sdk_utils::OK) {
                return profile;
            }
            PacketCursor cursor(extractor.get());
            AudioBuffer::AudioBufferPtr packet;
            while (cursor.nextPacket(packet)) {
                ++profile.count;
                profile.maxPacketBytes = std::max(profile.maxPacketBytes, packet->size());
            }
            return profile;
        }

        // 旧全量供给形式（反例）：整首压缩负载常驻、一次性交给解码器。
        // 它必须被两条判据同时判红，否则判据不能区分新旧实现。
        class WholeViewFlacExtractor : public ExtractorHelper {
        public:
            WholeViewFlacExtractor(ExtractorHelper *inner, const std::string &path)
                : m_inner(inner)
            {
                FileSource source(path.c_str());
                off64_t size = 0;
                if (source.initCheck() != sdk_utils::OK || source.getSize(&size) != sdk_utils::OK
                    || size <= 0) {
                    return;
                }
                m_block = std::make_shared<AudioBuffer>(static_cast<size_t>(size));
                if (source.readAt(0, m_block->data(), static_cast<size_t>(size))
                    != static_cast<ssize_t>(size)) {
                    m_block.reset();
                }
            }

            sdk_utils::status_t initCheck() override
            {
                return m_block != nullptr ? sdk_utils::OK : sdk_utils::INVALID_OPERATION;
            }
            AudioSpec getAudioSpec() override { return m_inner->getAudioSpec(); }
            AudioCodecID getAudioCodecID() override { return m_inner->getAudioCodecID(); }
            // 一包就是整首：字节定位只有「还没交出」与「已经全部交出」两态。
            // 交出之后位置=整首长度，解码侧再减去「本包内还没读走的字节」，与逐包形式同一口径。
            off64_t getDataSize() override
            {
                return m_block != nullptr ? static_cast<off64_t>(m_block->size()) : 0;
            }
            off64_t packetBytePosition() const override
            {
                return (m_delivered && m_block != nullptr) ? static_cast<off64_t>(m_block->size())
                                                           : 0;
            }
            size_t packetCount() override { return 1; }
            AudioBuffer::AudioBufferPtr packetAt(size_t index) override
            {
                if (index != 0 || m_block == nullptr) {
                    return nullptr;
                }
                m_delivered = true;
                return m_block;
            }
            size_t heldPayloadBytes() const override
            {
                return m_block != nullptr ? m_block->size() : 0;
            }

        private:
            ExtractorHelper *m_inner;
            AudioBuffer::AudioBufferPtr m_block;
            bool m_delivered = false;
        };
    } // namespace

    TEST_CASE("flac stays inside the held bytes cap and decodes in parallel with delivery")
    {
        RealMediaFixture fixture;
        if (!fixture.exists(kFlac)) {
            std::printf("[SKIP] %s (素材未随仓库下发)\n", kFlac);
            return;
        }

        const PacketProfile profile = measurePacketProfile(fixture.mediaDir(), kFlac);
        REQUIRE(profile.count > 1000);
        REQUIRE(profile.maxPacketBytes > 0);
        // 增量实现下「一包」就是一帧：远小于整首 32 MB。
        REQUIRE(profile.maxPacketBytes < 64 * 1024);

        const PlaybackObservation observation
            = playWholeTrack(fixture.mediaDir(), kFlac, ".flac", 300000);
        REQUIRE(observation.opened);
        REQUIRE(observation.endState == StreamDecoderState::EOS);

        const size_t packetBudget = profile.maxPacketBytes * 2;
        const size_t upper        = AudioRingBuffer::kDefaultCapacity + packetBudget
            + kAvioHeadroomBytes;
        CHECK_MESSAGE(observation.metrics.peakHeldBytes <= upper,
                      "常驻峰值 " << observation.metrics.peakHeldBytes << " 超过上界 " << upper);

        // 并行：首帧时至少拉过一包（走的是包序列），且序列尚未耗尽（不是先解完才交付）。
        CHECK_MESSAGE(observation.metrics.packetsPulledAtFirstPcm >= 1,
                      "首帧交付时供给序列还没开始");
        CHECK_MESSAGE(observation.metrics.packetsPulledAtFirstPcm
                          < observation.metrics.packetsPulled,
                      "首帧交付时供给序列已经耗尽：解码与消费没有并行");

        MESSAGE("flac cap: maxPacket " << profile.maxPacketBytes << " -> upper " << upper
                                       << ", peak " << observation.metrics.peakHeldBytes
                                       << "; firstPcm packets "
                                       << observation.metrics.packetsPulledAtFirstPcm << "/"
                                       << observation.metrics.packetsPulled);
    }

    TEST_CASE("whole view supply is red under both criteria")
    {
        RealMediaFixture fixture;
        if (!fixture.exists(kFlac)) {
            std::printf("[SKIP] %s (素材未随仓库下发)\n", kFlac);
            return;
        }

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> inner;
        REQUIRE(fixture.create(kFlac, ".flac", source, inner));
        REQUIRE(inner != nullptr);

        WholeViewFlacExtractor wholeView(inner.get(), fixture.mediaPath(kFlac));
        REQUIRE(wholeView.initCheck() == sdk_utils::OK);

        const PlaybackObservation observation = playWithExtractor(&wholeView, 300000);
        REQUIRE(observation.opened);
        REQUIRE(observation.endState == StreamDecoderState::EOS);
        // 解出来的 PCM 与增量形式逐字节相同：两条判据判的不是解码结果，是供给形式。
        CHECK(observation.bytes == 52513104);

        const PacketProfile profile = measurePacketProfile(fixture.mediaDir(), kFlac);
        const size_t upper          = AudioRingBuffer::kDefaultCapacity
            + profile.maxPacketBytes * 2 + kAvioHeadroomBytes;
        CHECK_MESSAGE(observation.metrics.peakHeldBytes > upper,
                      "整块供给没有被常驻上界判红：peak " << observation.metrics.peakHeldBytes
                                                          << " <= upper " << upper);
        CHECK_MESSAGE(observation.metrics.packetsPulledAtFirstPcm == 1,
                      "整块供给没有被并行判据判红：firstPcm 拉到的包数 "
                          << observation.metrics.packetsPulledAtFirstPcm);
        CHECK_MESSAGE(observation.metrics.packetsPulled == 1,
                      "整块供给的包序列不止一包：" << observation.metrics.packetsPulled);

        MESSAGE("whole view: peak " << observation.metrics.peakHeldBytes << " (upper " << upper
                                    << "), packets at first pcm "
                                    << observation.metrics.packetsPulledAtFirstPcm << "/"
                                    << observation.metrics.packetsPulled);
    }

    // FLAC 的增量切片（票面 #60 / 规格第 9 节 E7）。这一条是穿过全链路的 tracer bullet：
    // play → init 只留 O(1) 摘要 → demux 在解码线程里创建 → 逐包 pull → 逐帧 PCM 写 ring
    // → 消费端取走 → EOS 汇总。判据用「整首解出多少字节」兜住等价性：
    // 设备规格与源规格一致（44100/2ch/16bit），所以不重采样，字节数逐位可算。
    TEST_CASE("flac plays to end through packet iteration")
    {
        RealMediaFixture fixture;
        const char *name = "小镇姑娘-陶喆.flac";
        if (!fixture.exists(name)) {
            std::printf("[SKIP] %s (素材未随仓库下发)\n", name);
            return;
        }

        const PlaybackObservation observation
            = playWholeTrack(fixture.mediaDir(), name, ".flac", 300000);
        REQUIRE(observation.opened);
        CHECK_MESSAGE(observation.endState == StreamDecoderState::EOS,
                      "FLAC 没有走到 EOS，状态 " << static_cast<int>(observation.endState));
        // STREAMINFO totalSamples 13128276 × 2 声道 × 2 字节
        CHECK(observation.bytes == 52513104);
        // 逐包供给的证据：一首 297 s 的曲目要拉上千包，不是「整首一包」。
        CHECK(observation.metrics.packetsPulled > 1000);
        MESSAGE(name << ": packets " << observation.metrics.packetsPulled
                     << ", peakHeldBytes " << observation.metrics.peakHeldBytes
                     << ", startToFirstPcmUs " << observation.metrics.startToFirstPcmUs
                     << ", deliveredBytes " << observation.bytes << ", error "
                     << static_cast<int>(observation.error) << " (" << observation.errorDetail
                     << ")");
    }

    TEST_CASE("silence fill counter only covers the silence windows")
    {
        RealMediaFixture fixture;
        if (!fixture.exists(kShortOgg)) {
            std::printf("[SKIP] %s (素材未随仓库下发)\n", kShortOgg);
            return;
        }

        // 稳态窗口：消费端没有欠流，计数必须是 0。
        // 顺带在 DECODING 里故意记一次「静音填充」——静默窗口之外的记账不许进计数器，
        // 否则计数器会退化成「消费端调了几次接口」而不是「窗口内交付了几块静音」。
        PlaybackPlan steady;
        steady.probeOutsideWindow = true;
        PlaybackObservation steadyRun
            = playWholeTrack(fixture.mediaDir(), kShortOgg, ".ogg", 60000, steady);
        REQUIRE(steadyRun.opened);
        CHECK_MESSAGE(steadyRun.metrics.silenceFills == 0,
                      "稳态窗口之外的静音记账被计进了计数器");

        // seek 窗口：解码器进入 SEEKING，消费端在这段时间交付静音，计数必须动。
        PlaybackPlan seeking;
        seeking.seekAfterBytes = 65536; // ≈ 0.37 s 的 PCM
        seeking.seekTargetMs   = 1500;
        PlaybackObservation seekRun
            = playWholeTrack(fixture.mediaDir(), kShortOgg, ".ogg", 60000, seeking);
        REQUIRE(seekRun.opened);
        CHECK_MESSAGE(seekRun.metrics.silenceFills > 0, "SEEKING 窗口里的静音填充没有计数");
        CHECK_MESSAGE(seekRun.bytes > 0, "seek 之后没有继续交付 PCM");
    }

    TEST_CASE("instrument readings are reproducible across two runs")
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
            { kShortOgg, ".ogg", 60000 },
            { kLongWma, ".wma", 180000 },
        };

        for (size_t i = 0; i < sizeof(tracks) / sizeof(tracks[0]); ++i) {
            INFO(std::string(tracks[i].fileName));
            const PlaybackObservation first
                = playWholeTrack(fixture.mediaDir(), tracks[i].fileName, tracks[i].extension,
                                 tracks[i].timeoutMs);
            const PlaybackObservation second
                = playWholeTrack(fixture.mediaDir(), tracks[i].fileName, tracks[i].extension,
                                 tracks[i].timeoutMs);
            REQUIRE(first.opened);
            REQUIRE(second.opened);

            // 计数型仪表必须逐位可复现；计时型只要求两次都记上了（时长受机器负载影响）。
            CHECK_MESSAGE(first.metrics.peakHeldBytes == second.metrics.peakHeldBytes,
                          "分配记账峰值两次不一致: " << first.metrics.peakHeldBytes << " vs "
                                                    << second.metrics.peakHeldBytes);
            CHECK_MESSAGE(first.metrics.silenceFills == second.metrics.silenceFills,
                          "静音填充次数两次不一致");
            CHECK(first.metrics.startToFirstPcmUs > 0);
            CHECK(second.metrics.startToFirstPcmUs > 0);
            CHECK(first.metrics.initScanUs > 0);
            CHECK(second.metrics.initScanUs > 0);

            MESSAGE(tracks[i].fileName << ": peakHeldBytes " << first.metrics.peakHeldBytes << " / "
                                       << second.metrics.peakHeldBytes << ", initScanUs "
                                       << first.metrics.initScanUs << " / "
                                       << second.metrics.initScanUs << ", startToFirstPcmUs "
                                       << first.metrics.startToFirstPcmUs << " / "
                                       << second.metrics.startToFirstPcmUs);
        }
    }
}
