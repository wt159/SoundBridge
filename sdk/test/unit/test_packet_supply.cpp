#include "../fixtures/RealMediaFixture.hpp"
#include <AudioBuffer.h>
#include <AudioCommon.hpp>
#include <AudioDecodeProcess.h>
#include <ExtractorFactory.h>
#include <ExtractorHelper.hpp>
#include <FileSource.h>
#include <algorithm>
#include <doctest/doctest.h>
#include <memory>
#include <string>

// 量测仪器（AudioDecodeProcess，测试专用）在 #66 contract 后只剩包工厂一条供给路径。
//
// 仪器用 PacketCursor 逐包拼出整段负载再解码，换算出的解码时长必须与提取器
// init 期给出的标称时长吻合（双向容差），且确实逐包交付过包。
// 生产链（AudioStreamDecoder::start()）走增量供给，不经过本仪器；
// getPacketizedMetaData() 按票面保留测试专用，用于比对旧逐包表（若非空）。

namespace {

struct DecodedSummary {
    size_t pcmBytes;
    uint64_t measuredMs;
    size_t packetCount;
    bool ok;
};

DecodedSummary decodeWithInstrument(ExtractorHelper *extractor)
{
    DecodedSummary summary = { 0, 0, 0, false };

    AudioDecodeProcess process(extractor);
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
    TEST_CASE("packet iteration instrument decodes the advertised duration through the packet factory")
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

            // 独立预言机：提取器 init 期给出的标称时长，不是仪器自己算出的量。
            const uint64_t declaredMs = extractor->getAudioSpec().durationMs;
            REQUIRE(declaredMs > 0);
            const uint64_t toleranceMs = std::max<uint64_t>(declaredMs / 100, 150);

            const DecodedSummary decoded = decodeWithInstrument(extractor.get());
            CHECK_MESSAGE(decoded.ok, "packet iteration instrument failed: " << name);
            CHECK_MESSAGE(decoded.measuredMs + toleranceMs >= declaredMs,
                          name << ": decoded duration too short, " << decoded.measuredMs
                               << "ms vs declared " << declaredMs << "ms");
            CHECK_MESSAGE(decoded.measuredMs <= declaredMs + toleranceMs,
                          name << ": decoded duration too long, " << decoded.measuredMs
                               << "ms vs declared " << declaredMs << "ms");

            // 包工厂确实逐包交付：有旧逐包表时逐项一致；没有时允许 #62 起的
            // 「播放期按 offset 现场切包」（mp3 / aac 逐帧）——它不建旧包表，
            // 但包数必须 ≥1，且上面的时长等价性仍然成立。
            const size_t legacyPackets = extractor->getPacketizedMetaData().size();
            CHECK_MESSAGE(decoded.packetCount >= 1,
                          name << ": packet iteration fed no packet at all");
            if (legacyPackets > 0) {
                CHECK_MESSAGE(decoded.packetCount == legacyPackets,
                              name << ": packet iteration fed " << decoded.packetCount
                                   << " packets, legacy view has " << legacyPackets);
            }
        }
    }
}
