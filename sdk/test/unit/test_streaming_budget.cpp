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
            // seek 精度（#61）：seek 之后的交付量单独记账，不与 seek 之前的字节混在一起。
            // 目标是「期望的已消费 PCM 时间」（与 positionMs 同源），所以量的是消费端拿到的 PCM。
            bool seekIssued                  = false;
            bool seekCompleted               = false;
            uint64_t firstPcmMsAfterSeek     = 0;
            size_t bytesAfterSeek            = 0;
        };

        // 把 ring 里可读的 PCM 全部取走；counter 非空时累加取走的字节数。
        size_t drainRing(AudioRingBuffer &ring, std::vector<char> &buf, size_t *counter)
        {
            size_t total = 0;
            while (ring.availableRead() > 0) {
                size_t once = std::min(buf.size(), ring.availableRead());
                size_t got  = ring.read(buf.data(), once);
                if (got == 0) {
                    break;
                }
                total += got;
            }
            if (counter != nullptr) {
                *counter += total;
            }
            return total;
        }
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
        // devSpec 默认是 44.1 kHz / 2ch / S16（与 FLAC 素材一致，不重采样）；
        // 素材规格不同时由调用方传入源规格，判据才能直接按字节数比对交付量。
        PlaybackObservation playWithExtractor(ExtractorHelper *extractor, int timeoutMs,
                                              const PlaybackPlan &plan = PlaybackPlan(),
                                              const AudioSpec &devSpec    = makeDevSpec())
        {
            PlaybackObservation observation;
            if (extractor == nullptr || extractor->initCheck() != sdk_utils::OK) {
                return observation;
            }

            AudioRingBuffer ring(AudioRingBuffer::kDefaultCapacity);
            AudioStreamDecoder decoder(&ring, devSpec);
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
            // seek 落地之前一律不读 ring：旧段的残留由解码线程 reset() 丢弃，
            // 不计入「seek 之后交付了多少」。落地信号 = 位置回到目标（段基准 + 段内读计数归零）。
            const bool hasSeek = plan.seekAfterBytes > 0;
            bool counting      = true;
            int elapsed        = 0;
            while (elapsed <= timeoutMs) {
                if (!seeked && hasSeek && observation.bytes >= plan.seekAfterBytes) {
                    seeked                 = true;
                    counting               = false;
                    observation.seekIssued = true;
                    decoder.seekToMs(plan.seekTargetMs);
                }

                if (!counting) {
                    if (isSilenceWindow(decoder)) {
                        decoder.noteSilenceFill();
                    }
                    if (decoder.positionMs() == plan.seekTargetMs) {
                        // 首帧时间：ring 已被解码线程 reset，此刻的位置就是目标段起点。
                        counting                        = true;
                        observation.seekCompleted       = true;
                        observation.firstPcmMsAfterSeek = decoder.positionMs();
                    }
                } else if (isSilenceWindow(decoder)) {
                    // 静默窗口里消费端交付静音：交给解码器记账（与生产同一个计数入口）。
                    decoder.noteSilenceFill();
                } else {
                    if (plan.probeOutsideWindow && !probed) {
                        probed = true;
                        decoder.noteSilenceFill();
                    }
                    // 只有「seek 已发出且已落地」之后读到的字节才算 seek 后的交付量。
                    size_t *afterSeek
                        = (seeked && counting) ? &observation.bytesAfterSeek : nullptr;
                    observation.bytes += drainRing(ring, buf, afterSeek);
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
                size_t *afterSeek = (seeked && counting) ? &observation.bytesAfterSeek : nullptr;
                observation.bytes += drainRing(ring, buf, afterSeek);
            }
            decoder.stop();
            return observation;
        }

        PlaybackObservation playWholeTrack(const std::string &mediaDir, const std::string &fileName,
                                          const char *extension, int timeoutMs,
                                          const PlaybackPlan &plan = PlaybackPlan(),
                                          const AudioSpec &devSpec = makeDevSpec())
        {
            const std::string path = mediaDir + "/" + fileName;

            FileSource source(path.c_str());
            if (source.initCheck() != sdk_utils::OK) {
                return PlaybackObservation();
            }
            // source 的生命周期必须覆盖整个解码过程：提取器只持它的裸指针。
            std::shared_ptr<ExtractorHelper> extractor(
                ExtractorFactory::createExtractor(&source, extension, true));
            return playWithExtractor(extractor.get(), timeoutMs, plan, devSpec);
        }

        // ring 里是设备格式（makeDevSpec）的字节，换算成毫秒。
        uint64_t devBytesToMs(size_t bytes)
        {
            const AudioSpec spec = makeDevSpec();
            const uint64_t denom = static_cast<uint64_t>(spec.sampleRate) * spec.numChannel
                * spec.bytesPerSample;
            return denom == 0 ? 0 : static_cast<uint64_t>(bytes) * 1000 / denom;
        }

        // 提取器 init 期给出的标称时长（播放前即可得）。
        uint64_t declaredDurationMs(const std::string &mediaDir, const std::string &fileName,
                                    const char *extension)
        {
            FileSource source((mediaDir + "/" + fileName).c_str());
            if (source.initCheck() != sdk_utils::OK) {
                return 0;
            }
            std::shared_ptr<ExtractorHelper> extractor(
                ExtractorFactory::createExtractor(&source, extension, true));
            if (extractor == nullptr) {
                return 0;
            }
            return extractor->getAudioSpec().durationMs;
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
        PacketProfile measurePacketProfile(const std::string &mediaDir, const char *name,
                                           const char *extension = ".flac")
        {
            PacketProfile profile;
            FileSource source((mediaDir + "/" + name).c_str());
            if (source.initCheck() != sdk_utils::OK) {
                return profile;
            }
            std::shared_ptr<ExtractorHelper> extractor(
                ExtractorFactory::createExtractor(&source, extension, true));
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

        // 素材的 O(1) 摘要（spec + 可播 PCM 字节数）：判据要拿它当「覆盖度基线」，
        // 不额外合成素材、也不从文件长度反推格式头。
        struct TrackFacts {
            AudioSpec spec;
            size_t payloadBytes = 0;
            bool ok             = false;
        };

        TrackFacts probeTrack(const std::string &mediaDir, const char *name, const char *extension)
        {
            TrackFacts facts;
            FileSource source((mediaDir + "/" + name).c_str());
            if (source.initCheck() != sdk_utils::OK) {
                return facts;
            }
            std::shared_ptr<ExtractorHelper> extractor(
                ExtractorFactory::createExtractor(&source, extension, true));
            if (extractor == nullptr || extractor->initCheck() != sdk_utils::OK) {
                return facts;
            }
            facts.spec = extractor->getAudioSpec();
            const size_t frameBytes = static_cast<size_t>(facts.spec.numChannel)
                * static_cast<size_t>(facts.spec.bytesPerSample);
            if (frameBytes == 0) {
                return facts;
            }
            facts.payloadBytes = static_cast<size_t>(facts.spec.samples) * frameBytes;
            facts.ok           = facts.payloadBytes > 0;
            return facts;
        }

        // 旧全量供给形式（反例）：整首负载（压缩或 PCM）常驻、一次性交给解码器。
        // 它必须被两条判据同时判红，否则判据不能区分新旧实现。
        class WholeViewExtractor : public ExtractorHelper {
        public:
            WholeViewExtractor(ExtractorHelper *inner, const std::string &path)
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

        const PacketProfile profile = measurePacketProfile(fixture.mediaDir(), kFlac, ".flac");
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

        WholeViewExtractor wholeView(inner.get(), fixture.mediaPath(kFlac));
        REQUIRE(wholeView.initCheck() == sdk_utils::OK);

        const PlaybackObservation observation = playWithExtractor(&wholeView, 300000);
        REQUIRE(observation.opened);
        REQUIRE(observation.endState == StreamDecoderState::EOS);
        // 解出来的 PCM 与增量形式逐字节相同：两条判据判的不是解码结果，是供给形式。
        CHECK(observation.bytes == 52513104);

        const PacketProfile profile = measurePacketProfile(fixture.mediaDir(), kFlac, ".flac");
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

        // 提取器跨任务存活（MusicPlayList 按曲目持有它），所以同一个提取器要连播两次：
        // 第二次必须仍从流头开始——包序列的起点不能停在上一任务读到的位置。
        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor;
        REQUIRE(fixture.create(name, ".flac", source, extractor));
        REQUIRE(extractor != nullptr);

        const PlaybackObservation first = playWithExtractor(extractor.get(), 300000);
        REQUIRE(first.opened);
        CHECK_MESSAGE(first.endState == StreamDecoderState::EOS,
                      "FLAC 没有走到 EOS，状态 " << static_cast<int>(first.endState));
        // STREAMINFO totalSamples 13128276 × 2 声道 × 2 字节
        CHECK(first.bytes == 52513104);
        // 逐包供给的证据：一首 297 s 的曲目要拉上千包，不是「整首一包」。
        CHECK(first.metrics.packetsPulled > 1000);

        const PlaybackObservation second = playWithExtractor(extractor.get(), 300000);
        REQUIRE(second.opened);
        CHECK(second.endState == StreamDecoderState::EOS);
        CHECK(second.bytes == 52513104);
        CHECK(second.metrics.packetsPulled == first.metrics.packetsPulled);

        MESSAGE(name << ": packets " << first.metrics.packetsPulled
                     << ", peakHeldBytes " << first.metrics.peakHeldBytes
                     << ", startToFirstPcmUs " << first.metrics.startToFirstPcmUs
                     << ", deliveredBytes " << first.bytes << ", replay bytes "
                     << second.bytes << ", error " << static_cast<int>(first.error) << " ("
                     << first.errorDetail << ")");
    }

    // mp3 的增量切片（票面 #62 / 规格第 9 节 E9）。判据与 FLAC 同形：
    // 包工厂逐帧供给、首帧交付时序列未耗尽（并行），峰值落在上界内。
    TEST_CASE("mp3 plays to end through packet iteration")
    {
        RealMediaFixture fixture;
        const char *name = "小镇姑娘-陶喆.128.mp3";
        if (!fixture.exists(name)) {
            std::printf("[SKIP] %s (素材未随仓库下发)\n", name);
            return;
        }

        const PacketProfile profile = measurePacketProfile(fixture.mediaDir(), name, ".mp3");
        REQUIRE(profile.count > 5000); // 逐帧，比 FLAC 高一个量级
        REQUIRE(profile.maxPacketBytes > 0);
        REQUIRE(profile.maxPacketBytes < 8 * 1024);

        const PlaybackObservation observation
            = playWholeTrack(fixture.mediaDir(), name, ".mp3", 300000);
        REQUIRE(observation.opened);
        CHECK_MESSAGE(observation.endState == StreamDecoderState::EOS,
                      "mp3 没有走到 EOS，状态 " << static_cast<int>(observation.endState)
                                                << " / " << observation.errorDetail);

        const size_t upper = AudioRingBuffer::kDefaultCapacity
            + profile.maxPacketBytes * 2 + kAvioHeadroomBytes;
        CHECK_MESSAGE(observation.metrics.peakHeldBytes <= upper,
                      "常驻峰值 " << observation.metrics.peakHeldBytes << " 超过上界 " << upper);
        CHECK(observation.metrics.packetsPulled >= 5000);
        CHECK(observation.metrics.packetsPulledAtFirstPcm >= 1);
        CHECK(observation.metrics.packetsPulledAtFirstPcm
              < observation.metrics.packetsPulled);

        const uint64_t declaredMs = declaredDurationMs(fixture.mediaDir(), name, ".mp3");
        const uint64_t deliveredMs = devBytesToMs(observation.bytes);
        const uint64_t tolerance
            = std::max<uint64_t>(declaredMs / 100, 150);
        REQUIRE(declaredMs > 0);
        CHECK_MESSAGE(deliveredMs + tolerance >= declaredMs,
                      "交付时长 " << deliveredMs << "ms 短于标称 " << declaredMs << "ms");
        CHECK_MESSAGE(deliveredMs <= declaredMs + tolerance,
                      "交付时长 " << deliveredMs << "ms 超过标称 " << declaredMs << "ms");

        MESSAGE("mp3: packets " << observation.metrics.packetsPulled << " (maxPacket "
                                 << profile.maxPacketBytes << ") peak "
                                 << observation.metrics.peakHeldBytes << " / upper " << upper
                                 << ", firstPcm packets "
                                 << observation.metrics.packetsPulledAtFirstPcm
                                 << ", delivered " << deliveredMs << "ms / declared "
                                 << declaredMs << "ms");
    }

    // aac 的增量切片（票面 #62）：每个 ADTS 帧一包。
    TEST_CASE("aac plays to end through packet iteration")
    {
        RealMediaFixture fixture;
        const char *name = "WavinFlag.aac";
        if (!fixture.exists(name)) {
            std::printf("[SKIP] %s (素材未随仓库下发)\n", name);
            return;
        }

        const PacketProfile profile = measurePacketProfile(fixture.mediaDir(), name, ".aac");
        REQUIRE(profile.count > 5000);
        REQUIRE(profile.maxPacketBytes > 0);
        REQUIRE(profile.maxPacketBytes < 16 * 1024);

        const PlaybackObservation observation
            = playWholeTrack(fixture.mediaDir(), name, ".aac", 300000);
        REQUIRE(observation.opened);
        CHECK_MESSAGE(observation.endState == StreamDecoderState::EOS,
                      "aac 没有走到 EOS，状态 " << static_cast<int>(observation.endState)
                                                << " / " << observation.errorDetail);

        const size_t upper = AudioRingBuffer::kDefaultCapacity
            + profile.maxPacketBytes * 2 + kAvioHeadroomBytes;
        CHECK_MESSAGE(observation.metrics.peakHeldBytes <= upper,
                      "常驻峰值 " << observation.metrics.peakHeldBytes << " 超过上界 " << upper);
        CHECK(observation.metrics.packetsPulled >= 5000);
        CHECK(observation.metrics.packetsPulledAtFirstPcm >= 1);
        CHECK(observation.metrics.packetsPulledAtFirstPcm
              < observation.metrics.packetsPulled);

        const uint64_t declaredMs = declaredDurationMs(fixture.mediaDir(), name, ".aac");
        const uint64_t deliveredMs = devBytesToMs(observation.bytes);
        const uint64_t tolerance
            = std::max<uint64_t>(declaredMs / 100, 150);
        REQUIRE(declaredMs > 0);
        CHECK_MESSAGE(deliveredMs + tolerance >= declaredMs,
                      "交付时长 " << deliveredMs << "ms 短于标称 " << declaredMs << "ms");
        CHECK_MESSAGE(deliveredMs <= declaredMs + tolerance,
                      "交付时长 " << deliveredMs << "ms 超过标称 " << declaredMs << "ms");

        MESSAGE("aac: packets " << observation.metrics.packetsPulled << " (maxPacket "
                                 << profile.maxPacketBytes << ") peak "
                                 << observation.metrics.peakHeldBytes << " / upper " << upper
                                 << ", firstPcm packets "
                                 << observation.metrics.packetsPulledAtFirstPcm
                                 << ", delivered " << deliveredMs << "ms / declared "
                                 << declaredMs << "ms");
    }

    // ogg（Vorbis）的增量路径（票面 #62）：init 不再保留页表，解码仍由
    // libvorbisfile 经 DataSource 回调推进，ov_time_seek 的 native RA 保持不变。
    TEST_CASE("ogg vorbis plays to end without a page table")
    {
        RealMediaFixture fixture;
        if (!fixture.exists(kShortOgg)) {
            std::printf("[SKIP] %s (素材未随仓库下发)\n", kShortOgg);
            return;
        }

        const PlaybackObservation observation
            = playWholeTrack(fixture.mediaDir(), kShortOgg, ".ogg", 60000);
        REQUIRE(observation.opened);
        CHECK_MESSAGE(observation.endState == StreamDecoderState::EOS,
                      "ogg 没有走到 EOS，状态 " << static_cast<int>(observation.endState)
                                                << " / " << observation.errorDetail);
        CHECK(observation.bytes > 0);
        // ogg 不走包工厂（native RA 由 libvorbisfile 负责），常驻只剩解码器的固定缓冲，
        // 远小于 ring + AVIO 这两项判据里的独立项。
        const size_t upper = AudioRingBuffer::kDefaultCapacity + kAvioHeadroomBytes;
        CHECK_MESSAGE(observation.metrics.peakHeldBytes <= upper,
                      "ogg 常驻峰值 " << observation.metrics.peakHeldBytes << " 超过 " << upper);

        const uint64_t declaredMs
            = declaredDurationMs(fixture.mediaDir(), kShortOgg, ".ogg");
        const uint64_t deliveredMs = devBytesToMs(observation.bytes);
        const uint64_t tolerance   = std::max<uint64_t>(declaredMs / 100, 150);
        REQUIRE(declaredMs > 0);
        CHECK(deliveredMs + tolerance >= declaredMs);
        CHECK(deliveredMs <= declaredMs + tolerance);

        MESSAGE("ogg: peak " << observation.metrics.peakHeldBytes << " / upper " << upper
                             << ", delivered " << deliveredMs << "ms / declared " << declaredMs
                             << "ms, packetsPulled "
                             << observation.metrics.packetsPulled);
    }

    // mp3 / aac 的 seek 走「顺序重放」档（#49 第二档）：重建解码器，从包序列起点
    // 重新迭代到目标、丢弃目标前的 PCM。峰值不随目标位置增长，native RA 只留给
    // FLAC / Vorbis。这条用例锁住重放分支不会把 seek 后的交付弄丢。
    TEST_CASE("aac seek replays from the stream head and keeps delivering")
    {
        RealMediaFixture fixture;
        const char *name = "48000_fltp_1.aac";
        if (!fixture.exists(name)) {
            std::printf("[SKIP] %s (素材未随仓库下发)\n", name);
            return;
        }

        PlaybackPlan plan;
        plan.seekAfterBytes = 65536; // ≈ 0.37 s 的设备 PCM
        plan.seekTargetMs   = 3000;
        const PlaybackObservation observation
            = playWholeTrack(fixture.mediaDir(), name, ".aac", 60000, plan);
        REQUIRE(observation.opened);
        CHECK_MESSAGE(observation.endState == StreamDecoderState::EOS,
                      "seek 之后没有走到 EOS，状态 " << static_cast<int>(observation.endState)
                                                     << " / " << observation.errorDetail);
        CHECK(observation.bytes > 0);

        // seek 生效的直接证据：目标之前的 PCM 被丢弃，交付时长明显短于整首。
        // （静默窗口的计数在 SEEKING 用例里判；Windows 的 5ms 轮询粒度可能整段错过
        //   12ms 窗口，拿它当 seek 证据会假红。）
        const uint64_t declaredMs = declaredDurationMs(fixture.mediaDir(), name, ".aac");
        const uint64_t deliveredMs = devBytesToMs(observation.bytes);
        REQUIRE(declaredMs > 3000);
        CHECK_MESSAGE(deliveredMs + 1000 <= declaredMs,
                      "seek 到 3000ms 之后仍交付了 " << deliveredMs << "ms / 标称 " << declaredMs
                                                     << "ms：目标之前的 PCM 没有丢弃");

        MESSAGE("aac seek: delivered " << deliveredMs << "ms / declared " << declaredMs
                                       << "ms, peak " << observation.metrics.peakHeldBytes
                                       << ", packets " << observation.metrics.packetsPulled);
    }

    // mkv 的增量切片（票面 #63 / 规格第 9 节 E10）。mkv 走 ffmpeg demux，包边界在
    // **播放期**按 offset 现场重算：init 只读扫描取标称时长后就把 demux 关掉，
    // 播放时重开一个、逐块拉、逐块丢，不保留逐包时间轴 / 页表 / seek 表。
    // 判据与 FLAC 同形：常驻上界 + 并行。
    TEST_CASE("mkv stays inside the held bytes cap and decodes in parallel with delivery")
    {
        RealMediaFixture fixture;
        const char *name = "music-mkv.mkv";
        if (!fixture.exists(name)) {
            std::printf("[SKIP] %s (素材未随仓库下发)\n", name);
            return;
        }

        const PacketProfile profile = measurePacketProfile(fixture.mediaDir(), name, ".mkv");
        REQUIRE(profile.count > 10);
        REQUIRE(profile.maxPacketBytes > 0);
        // 一块一包：远小于整份 payload（789 KB），绝不是「整首一包」。
        REQUIRE(profile.maxPacketBytes < 64 * 1024);

        const PlaybackObservation observation
            = playWholeTrack(fixture.mediaDir(), name, ".mkv", 120000);
        REQUIRE(observation.opened);
        CHECK_MESSAGE(observation.endState == StreamDecoderState::EOS,
                      "mkv 没有走到 EOS，状态 " << static_cast<int>(observation.endState) << " / "
                                                << observation.errorDetail);
        CHECK(observation.bytes > 0);

        const size_t upper = AudioRingBuffer::kDefaultCapacity
            + profile.maxPacketBytes * 2 + kAvioHeadroomBytes;
        CHECK_MESSAGE(observation.metrics.peakHeldBytes <= upper,
                      "常驻峰值 " << observation.metrics.peakHeldBytes << " 超过上界 " << upper);

        // 并行：首帧时至少拉过一包，且序列尚未耗尽（不是先解完才交付）。
        CHECK(observation.metrics.packetsPulled >= profile.count);
        CHECK(observation.metrics.packetsPulledAtFirstPcm >= 1);
        CHECK(observation.metrics.packetsPulledAtFirstPcm < observation.metrics.packetsPulled);

        // 标称时长与覆盖度基线一致：容器声明字段写错（393.216 s）时不能改口径，
        // 交付时长必须落在 24.576 s 的双向容差内（MediaCoverage 的同一判据）。
        const uint64_t declaredMs = declaredDurationMs(fixture.mediaDir(), name, ".mkv");
        const uint64_t deliveredMs = devBytesToMs(observation.bytes);
        const uint64_t tolerance   = std::max<uint64_t>(declaredMs / 100, 150);
        REQUIRE(declaredMs == 24576);
        CHECK_MESSAGE(deliveredMs + tolerance >= declaredMs,
                      "交付时长 " << deliveredMs << "ms 短于标称 " << declaredMs << "ms");
        CHECK_MESSAGE(deliveredMs <= declaredMs + tolerance,
                      "交付时长 " << deliveredMs << "ms 超过标称 " << declaredMs << "ms");

        // 现场重算的 CPU 成本是独立指标（不设阈值），只要求确实记上了。
        CHECK(observation.metrics.packetBoundaryMicros > 0);

        MESSAGE("mkv: packets " << observation.metrics.packetsPulled << " (maxPacket "
                                << profile.maxPacketBytes << "), peak "
                                << observation.metrics.peakHeldBytes << " / upper " << upper
                                << ", firstPcm packets "
                                << observation.metrics.packetsPulledAtFirstPcm << ", delivered "
                                << deliveredMs << "ms / declared " << declaredMs
                                << "ms, boundaryMicros "
                                << observation.metrics.packetBoundaryMicros);
    }

    // 提取器跨任务存活（MusicPlayList 按曲目持有它）：同一个提取器连播两次，第二次必须
    // 仍从流头开始——包序列的起点不能停在上一任务读到的位置。
    TEST_CASE("mkv replays from the stream head for a second playback task")
    {
        RealMediaFixture fixture;
        const char *name = "music-mkv.mkv";
        if (!fixture.exists(name)) {
            std::printf("[SKIP] %s (素材未随仓库下发)\n", name);
            return;
        }

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor;
        REQUIRE(fixture.create(name, ".mkv", source, extractor));
        REQUIRE(extractor != nullptr);

        const PlaybackObservation first = playWithExtractor(extractor.get(), 120000);
        REQUIRE(first.opened);
        CHECK(first.endState == StreamDecoderState::EOS);
        CHECK(first.bytes > 0);

        const PlaybackObservation second = playWithExtractor(extractor.get(), 120000);
        REQUIRE(second.opened);
        CHECK(second.endState == StreamDecoderState::EOS);
        CHECK(second.bytes == first.bytes);
        CHECK(second.metrics.packetsPulled == first.metrics.packetsPulled);
    }

    // mkv 的 seek 走「顺序重放」档（#49 第二档）：重建解码器、从包序列起点重新迭代到目标，
    // 目标之前的 PCM 由解码器丢弃。峰值不随目标位置增长，也不新建整首 payload 缓存。
    TEST_CASE("mkv seek replays from the stream head and keeps delivering")
    {
        RealMediaFixture fixture;
        const char *name = "music-mkv.mkv";
        if (!fixture.exists(name)) {
            std::printf("[SKIP] %s (素材未随仓库下发)\n", name);
            return;
        }

        PlaybackPlan plan;
        plan.seekAfterBytes = 65536; // ≈ 0.37 s 的设备 PCM
        plan.seekTargetMs   = 5000;
        const PlaybackObservation observation
            = playWholeTrack(fixture.mediaDir(), name, ".mkv", 120000, plan);
        REQUIRE(observation.opened);
        CHECK_MESSAGE(observation.endState == StreamDecoderState::EOS,
                      "seek 之后没有走到 EOS，状态 " << static_cast<int>(observation.endState)
                                                     << " / " << observation.errorDetail);
        CHECK(observation.bytes > 0);

        // seek 生效的直接证据：目标之前的 PCM 被丢弃，交付时长明显短于整首。
        const uint64_t declaredMs  = declaredDurationMs(fixture.mediaDir(), name, ".mkv");
        const uint64_t deliveredMs = devBytesToMs(observation.bytes);
        REQUIRE(declaredMs > 5000);
        CHECK_MESSAGE(deliveredMs + 1000 <= declaredMs,
                      "seek 到 5000ms 之后仍交付了 " << deliveredMs << "ms / 标称 " << declaredMs
                                                     << "ms：目标之前的 PCM 没有丢弃");
    }

    TEST_CASE("silence fill counter only covers the silence windows")    {
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

    // —— wav / aiff 的增量切片（票面 #61 / 规格第 9 节 E8）——
    //
    // 原始 PCM 没有帧头可认，包边界只能是「定长切片」（对齐到帧），解码侧逐包写 ring。
    // 两条判据与前两条同源：常驻上界（262144 + P + 64KB + O(1)）与并行（首帧时序列未耗尽）。
    // 这两种素材的规格不是 44.1 kHz / 2ch，所以把源规格当设备规格传入：不重采样，
    // 交付的 PCM 字节数与素材的可播字节逐位相等，「覆盖度基线」因此可以按字节断言。
    namespace
    {
        struct PcmCase {
            const char *name;
            const char *extension;
        };

        const PcmCase kPcmCases[] = {
            { "music.wav", ".wav" },                          // 16 kHz / 单声道 / 16 bit，24.9 s
            { "俱乐部音乐循环-Freesound.aiff", ".aiff" },     // 44.1 kHz / 双声道 / 16 bit，27.4 s
        };
        const size_t kPcmCaseCount = sizeof(kPcmCases) / sizeof(kPcmCases[0]);
    } // namespace

    TEST_CASE("wav and aiff stay inside the held bytes cap and decode in parallel with delivery")
    {
        RealMediaFixture fixture;

        for (size_t i = 0; i < kPcmCaseCount; ++i) {
            const std::string name(kPcmCases[i].name);
            INFO(name);
            if (!fixture.exists(name)) {
                MESSAGE("[SKIP] " << name << " (素材未随仓库下发)");
                continue;
            }

            const TrackFacts facts = probeTrack(fixture.mediaDir(), name.c_str(),
                                                kPcmCases[i].extension);
            REQUIRE_MESSAGE(facts.ok, name << ": 提取器没有给出可播负载");
            const PacketProfile profile = measurePacketProfile(fixture.mediaDir(), name.c_str(),
                                                               kPcmCases[i].extension);
            REQUIRE(profile.count > 1);
            REQUIRE(profile.maxPacketBytes > 0);
            // 单包是定长切片，不是「整首一包」：P 由切片长度决定，与素材时长无关。
            REQUIRE(profile.maxPacketBytes <= 64 * 1024);

            const PlaybackObservation observation = playWholeTrack(
                fixture.mediaDir(), name, kPcmCases[i].extension, 120000, PlaybackPlan(),
                facts.spec);
            REQUIRE(observation.opened);
            REQUIRE_MESSAGE(observation.endState == StreamDecoderState::EOS,
                            name << ": 没有走到 EOS，状态 " << static_cast<int>(observation.endState)
                                 << "，错误 " << static_cast<int>(observation.error) << " ("
                                 << observation.errorDetail << ")");

            const size_t packetBudget = profile.maxPacketBytes * 2;
            const size_t upper        = AudioRingBuffer::kDefaultCapacity + packetBudget
                + kAvioHeadroomBytes;
            CHECK_MESSAGE(observation.metrics.peakHeldBytes <= upper,
                          name << ": 常驻峰值 " << observation.metrics.peakHeldBytes
                               << " 超过上界 " << upper);

            // 并行：首帧交付时至少拉过一包，且序列尚未耗尽（不是先整段读完才交付）。
            CHECK_MESSAGE(observation.metrics.packetsPulledAtFirstPcm >= 1,
                          name << ": 首帧交付时供给序列还没开始");
            CHECK_MESSAGE(observation.metrics.packetsPulledAtFirstPcm
                              < observation.metrics.packetsPulled,
                          name << ": 首帧交付时供给序列已经耗尽：解码与消费没有并行");
            // 整段按包走完：拉到的包数与包形状实测一致。
            CHECK_MESSAGE(observation.metrics.packetsPulled == profile.count,
                          name << ": 拉到 " << observation.metrics.packetsPulled << " 包，实测 "
                               << profile.count << " 包");
            // 覆盖度基线：未压缩，交付的字节数必须与素材的可播字节逐位相等。
            CHECK_MESSAGE(observation.bytes == facts.payloadBytes,
                          name << ": 交付 " << observation.bytes << " 字节，覆盖度基线 "
                               << facts.payloadBytes);

            MESSAGE(name << ": packets " << profile.count << ", maxPacket "
                         << profile.maxPacketBytes << " -> upper " << upper << ", peak "
                         << observation.metrics.peakHeldBytes << "; firstPcm packets "
                         << observation.metrics.packetsPulledAtFirstPcm << "/"
                         << observation.metrics.packetsPulled << ", delivered "
                         << observation.bytes);
        }
    }

    // 反例（判据的区分度）：整块供给的 wav 必须被常驻上界与并行同时判红。
    TEST_CASE("whole view supply of wav is red under the held bytes cap")
    {
        RealMediaFixture fixture;
        const char *name = "music.wav";
        if (!fixture.exists(name)) {
            std::printf("[SKIP] %s (素材未随仓库下发)\n", name);
            return;
        }

        const TrackFacts facts = probeTrack(fixture.mediaDir(), name, ".wav");
        REQUIRE(facts.ok);

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> inner;
        REQUIRE(fixture.create(name, ".wav", source, inner));
        REQUIRE(inner != nullptr);

        WholeViewExtractor wholeView(inner.get(), fixture.mediaPath(name));
        REQUIRE(wholeView.initCheck() == sdk_utils::OK);

        const PlaybackObservation observation
            = playWithExtractor(&wholeView, 120000, PlaybackPlan(), facts.spec);
        REQUIRE(observation.opened);
        REQUIRE(observation.endState == StreamDecoderState::EOS);
        // 整块替身从文件偏移 0 喂（wav 的 RIFF 头也在里面），所以交付量比 payload 多一个头长；
        // 判据判的不是解码结果，而是供给形式——下面两条才是这条用例的目的。
        CHECK(observation.bytes >= facts.payloadBytes);

        const PacketProfile profile = measurePacketProfile(fixture.mediaDir(), name, ".wav");
        const size_t upper          = AudioRingBuffer::kDefaultCapacity
            + profile.maxPacketBytes * 2 + kAvioHeadroomBytes;
        CHECK_MESSAGE(observation.metrics.peakHeldBytes > upper,
                      "整块供给没有被常驻上界判红：peak " << observation.metrics.peakHeldBytes
                                                          << " <= upper " << upper);
        CHECK_MESSAGE(observation.metrics.packetsPulledAtFirstPcm == 1,
                      "整块供给没有被并行判据判红：firstPcm 拉到的包数 "
                          << observation.metrics.packetsPulledAtFirstPcm);
    }

    // 生产链的设备规格通常不是素材规格（16 kHz 单声道 wav → 44.1 kHz 双声道设备）。
    // 增量路径与旧形式共用同一条「PCM → 重采样 → 写 ring」的口径，这里用默认设备规格
    // 跑一遍，确认逐包供给下重采样照常工作、EOS 照常收口。
    TEST_CASE("wav plays through the incremental path with device resampling")
    {
        RealMediaFixture fixture;
        const char *name = "music.wav";
        if (!fixture.exists(name)) {
            std::printf("[SKIP] %s (素材未随仓库下发)\n", name);
            return;
        }

        const TrackFacts facts = probeTrack(fixture.mediaDir(), name, ".wav");
        REQUIRE(facts.ok);
        REQUIRE(facts.spec.sampleRate != makeDevSpec().sampleRate);

        const PlaybackObservation observation
            = playWholeTrack(fixture.mediaDir(), name, ".wav", 120000);
        REQUIRE(observation.opened);
        REQUIRE_MESSAGE(observation.endState == StreamDecoderState::EOS,
                        "重采样路径没有走到 EOS，状态 " << static_cast<int>(observation.endState));

        // 重采样只改 PCM 的密度：输出帧数按采样率比例放大，帧长取设备规格。
        const AudioSpec dev        = makeDevSpec();
        const uint64_t frames      = static_cast<uint64_t>(facts.spec.samples);
        const uint64_t expected    = frames * dev.sampleRate * dev.numChannel * dev.bytesPerSample
            / static_cast<uint64_t>(facts.spec.sampleRate);
        const uint64_t tolerance   = expected / 100; // 1%，覆盖重采样的帧数截断
        CHECK_MESSAGE(observation.bytes + tolerance >= expected,
                      "重采样后的交付量 " << observation.bytes << " 少于期望 " << expected);
        CHECK_MESSAGE(observation.bytes <= expected + tolerance,
                      "重采样后的交付量 " << observation.bytes << " 多于期望 " << expected);

        MESSAGE(name << ": resampled delivered " << observation.bytes << " / expected "
                     << expected << ", packets " << observation.metrics.packetsPulled);
    }

    // 顺序重放档的 seek（规格第 6 节 / #49）：重启包序列、从起点按包迭代到目标、
    // 丢掉目标前的 PCM。精度沿用既有口径 max(target × 1%, 150ms)——不新造数值。
    TEST_CASE("sequential replay seek lands on the target for wav and aiff")
    {
        RealMediaFixture fixture;

        for (size_t i = 0; i < kPcmCaseCount; ++i) {
            const std::string name(kPcmCases[i].name);
            INFO(name);
            if (!fixture.exists(name)) {
                MESSAGE("[SKIP] " << name << " (素材未随仓库下发)");
                continue;
            }

            const TrackFacts facts = probeTrack(fixture.mediaDir(), name.c_str(),
                                                kPcmCases[i].extension);
            REQUIRE(facts.ok);
            REQUIRE(facts.spec.durationMs > 0);

            PlaybackPlan plan;
            plan.seekAfterBytes = facts.payloadBytes / 4;
            plan.seekTargetMs   = facts.spec.durationMs / 2;

            const PlaybackObservation observation = playWholeTrack(
                fixture.mediaDir(), name, kPcmCases[i].extension, 120000, plan, facts.spec);
            REQUIRE(observation.opened);
            REQUIRE_MESSAGE(observation.seekCompleted,
                            name << ": seek 没有落地，状态 "
                                 << static_cast<int>(observation.endState) << "，错误 "
                                 << static_cast<int>(observation.error));
            REQUIRE_MESSAGE(observation.endState == StreamDecoderState::EOS,
                            name << ": seek 之后没有走到 EOS，状态 "
                                 << static_cast<int>(observation.endState));

            const uint64_t toleranceMs = std::max<uint64_t>(plan.seekTargetMs / 100, 150);
            const uint64_t firstMs     = observation.firstPcmMsAfterSeek;
            CHECK_MESSAGE(firstMs + toleranceMs >= plan.seekTargetMs,
                          name << ": seek 后首帧时间 " << firstMs << " ms 早于目标 "
                               << plan.seekTargetMs << " ms 超过 " << toleranceMs << " ms");
            CHECK_MESSAGE(firstMs <= plan.seekTargetMs + toleranceMs,
                          name << ": seek 后首帧时间 " << firstMs << " ms 晚于目标 "
                               << plan.seekTargetMs << " ms 超过 " << toleranceMs << " ms");

            // 交付字节数：从目标点到结尾的期望字节数（按与解码器同一口径换算）。
            const uint64_t bytesPerMs = static_cast<uint64_t>(facts.spec.sampleRate)
                * static_cast<uint64_t>(facts.spec.numChannel)
                * static_cast<uint64_t>(facts.spec.bytesPerSample) / 1000;
            REQUIRE(bytesPerMs > 0);
            const uint64_t targetBytes  = plan.seekTargetMs * bytesPerMs;
            const size_t expectedAfter  = facts.payloadBytes - static_cast<size_t>(targetBytes);
            const size_t toleranceBytes = static_cast<size_t>(toleranceMs * bytesPerMs);
            const size_t delivered      = observation.bytesAfterSeek;
            CHECK_MESSAGE(delivered + toleranceBytes >= expectedAfter,
                          name << ": seek 后交付 " << delivered << " 字节，少于期望 " << expectedAfter
                               << "（容差 " << toleranceBytes << "）");
            CHECK_MESSAGE(delivered <= expectedAfter + toleranceBytes,
                          name << ": seek 后交付 " << delivered << " 字节，多于期望 " << expectedAfter
                               << "（容差 " << toleranceBytes << "）");

            MESSAGE(name << ": seek " << plan.seekTargetMs << " ms, firstPcm " << firstMs
                         << " ms, delivered after seek " << delivered << " / expected "
                         << expectedAfter << ", total " << observation.bytes << ", packets "
                         << observation.metrics.packetsPulled);
        }
    }

    // —— 最后一批「不可增量」格式（票面 #64 / 规格第 9 节 E11）——
    // m4a（AAC + ALAC）/ ape / asf / wma 复用 mkv 的现场重算机制：init 只读扫描只留
    // O(1) 摘要，播放期由各容器的 Reader 重开 demux、按 offset 逐包给出边界。
    // 判据与 mkv 用例同形：常驻上界 + 并行 + EOS + 覆盖度 + 现场重算成本。
    namespace
    {
        void checkBoundaryRecomputedFormat(const RealMediaFixture &fixture, const char *name,
                                          const char *extension, int timeoutMs)
        {
            const std::string fileName(name);
            INFO(fileName);
            if (!fixture.exists(fileName)) {
                MESSAGE("[SKIP] " << fileName << " (素材未随仓库下发)");
                return;
            }

            const PacketProfile profile
                = measurePacketProfile(fixture.mediaDir(), name, extension);
            REQUIRE(profile.count > 1);
            REQUIRE(profile.maxPacketBytes > 0);
            REQUIRE(profile.maxPacketBytes < 64 * 1024);

            const PlaybackObservation observation
                = playWholeTrack(fixture.mediaDir(), fileName, extension, timeoutMs);
            REQUIRE(observation.opened);
            REQUIRE_MESSAGE(observation.endState == StreamDecoderState::EOS,
                            fileName << ": 没有走到 EOS，状态 "
                                     << static_cast<int>(observation.endState) << " / "
                                     << observation.errorDetail);
            CHECK(observation.bytes > 0);

            const size_t upper = AudioRingBuffer::kDefaultCapacity
                + profile.maxPacketBytes * 2 + kAvioHeadroomBytes;
            CHECK_MESSAGE(observation.metrics.peakHeldBytes <= upper,
                          fileName << ": 常驻峰值 " << observation.metrics.peakHeldBytes
                                   << " 超过上界 " << upper);

            CHECK(observation.metrics.packetsPulled >= profile.count);
            CHECK_MESSAGE(observation.metrics.packetsPulledAtFirstPcm >= 1,
                          fileName << ": 首帧交付时供给序列还没开始");
            CHECK_MESSAGE(observation.metrics.packetsPulledAtFirstPcm
                              < observation.metrics.packetsPulled,
                          fileName << ": 首帧交付时供给序列已经耗尽：解码与消费没有并行");
            CHECK(observation.metrics.packetBoundaryMicros > 0);

            const uint64_t declaredMs
                = declaredDurationMs(fixture.mediaDir(), fileName, extension);
            const uint64_t deliveredMs = devBytesToMs(observation.bytes);
            const uint64_t tolerance   = RealMediaFixture::coverageToleranceMs(declaredMs);
            REQUIRE(declaredMs > 0);
            CHECK(deliveredMs + tolerance >= declaredMs);
            CHECK(deliveredMs <= declaredMs + tolerance);

            MESSAGE(fileName << ": packets " << profile.count << " (maxPacket "
                             << profile.maxPacketBytes << "), peak "
                             << observation.metrics.peakHeldBytes << " / upper " << upper
                             << ", firstPcm packets "
                             << observation.metrics.packetsPulledAtFirstPcm << "/"
                             << observation.metrics.packetsPulled << ", delivered "
                             << deliveredMs << "ms / declared " << declaredMs
                             << "ms, boundaryMicros "
                             << observation.metrics.packetBoundaryMicros);
        }
    } // namespace

    TEST_CASE("m4a aac stays inside the held bytes cap and decodes in parallel with delivery")
    {
        RealMediaFixture fixture;
        checkBoundaryRecomputedFormat(fixture, "摇滚乐_Freesound.m4a", ".m4a", 120000);
    }

    TEST_CASE("m4a alac stays inside the held bytes cap and decodes in parallel with delivery")
    {
        RealMediaFixture fixture;
        checkBoundaryRecomputedFormat(fixture, "music-alac.m4a", ".m4a", 120000);
    }
}
