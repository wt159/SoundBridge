#include "../fixtures/RealMediaFixture.hpp"
#include <AudioBuffer.h>
#include <AudioCommon.hpp>
#include <AudioDecodeProcess.h>
#include <ExtractorFactory.h>
#include <ExtractorHelper.hpp>
#include <FileSource.h>
#include <doctest/doctest.h>
#include <memory>
#include <string>

// 量测仪器（AudioDecodeProcess，测试专用）的两条供给路径等价性（票面 #57）。
//
// 旧形式：整块视图——提取器按需拼一份 getMetaData()，仪器按旧方式解码。
// 新形式：单包迭代——仪器被 PacketCursor 逐包喂，拼出整段再解码。
// 两条路径对同一曲目必须解出同样的 PCM 字节数，换算出的解码时长也一致。
//
// 生产链（AudioStreamDecoder::start()）本票不迁移，仍然走旧形式；本文件只证明
// 新形式已经能覆盖同一段负载，为 #60 起的格式迁移留好形状。

namespace {

struct DecodedSummary {
    size_t pcmBytes;
    uint64_t measuredMs;
    size_t packetCount;
    bool ok;
};

DecodedSummary decodeWithInstrument(ExtractorHelper *extractor,
                                    AudioDecodeProcess::PayloadSupply supply)
{
    DecodedSummary summary = { 0, 0, 0, false };

    AudioDecodeProcess process(extractor, supply);
    if (process.initCheck() != sdk_utils::OK) {
        return summary;
    }

    AudioBuffer::AudioBufferPtr pcm = process.getDecodeBuffer();
    if (pcm == nullptr || pcm->size() == 0) {
        return summary;
    }

    const AudioSpec decoded = process.getDecodeSpec();
    if (decoded.sampleRate <= 0 || decoded.numChannel <= 0 || decoded.bytesPerSample <= 0) {
        return summary;
    }

    const uint64_t frameBytes = static_cast<uint64_t>(decoded.numChannel)
                                * static_cast<uint64_t>(decoded.bytesPerSample);
    summary.pcmBytes   = pcm->size();
    summary.measuredMs
        = static_cast<uint64_t>(summary.pcmBytes) / frameBytes * 1000ULL
          / static_cast<uint64_t>(decoded.sampleRate);
    summary.packetCount = process.payloadPacketCount();
    summary.ok           = true;
    return summary;
}

} // namespace

TEST_SUITE("PacketSupply")
{
    TEST_CASE("two supply paths decode the same pcm bytes and duration")
    {
        RealMediaFixture fixture;
        const struct {
            const char *fileName;
            const char *extension;
        } cases[] = {
            { "music-ape.ape", ".ape" },
            { "像我这样的人-毛不易.128.mp3", ".mp3" },
        };

        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            const std::string name(cases[i].fileName);
            if (!fixture.exists(name)) {
                MESSAGE("[SKIP] " << name << " (素材未随仓库下发)");
                continue;
            }

            std::shared_ptr<FileSource> source;
            std::unique_ptr<ExtractorHelper> extractor;
            REQUIRE(fixture.create(name, cases[i].extension, source, extractor));
            REQUIRE(extractor != nullptr);
            REQUIRE(extractor->initCheck() == sdk_utils::OK);

            const DecodedSummary wholeView
                = decodeWithInstrument(extractor.get(),
                                       AudioDecodeProcess::PayloadSupply::WholeView);
            const DecodedSummary packetIteration
                = decodeWithInstrument(extractor.get(),
                                       AudioDecodeProcess::PayloadSupply::PacketIteration);

            CHECK_MESSAGE(wholeView.ok, "legacy instrument failed: " << name);
            CHECK_MESSAGE(packetIteration.ok, "packet iteration instrument failed: " << name);
            CHECK_MESSAGE(wholeView.pcmBytes == packetIteration.pcmBytes,
                          name << ": legacy vs packet iteration pcm bytes, " << wholeView.pcmBytes << " vs "
                               << packetIteration.pcmBytes);
            CHECK_MESSAGE(wholeView.measuredMs == packetIteration.measuredMs,
                          name << ": legacy vs packet iteration duration, " << wholeView.measuredMs
                               << "ms vs " << packetIteration.measuredMs << "ms");
            // 新路径确实逐包走过：包数等于旧逐包表（没有逐包表时为 1 整块）。
            const size_t legacyPackets = extractor->getPacketizedMetaData().size();
            const size_t expectedPackets = legacyPackets > 0 ? legacyPackets : 1;
            CHECK_MESSAGE(packetIteration.packetCount == expectedPackets,
                          name << ": packet iteration fed " << packetIteration.packetCount
                               << " packets, legacy view has " << expectedPackets);
        }
    }
}
