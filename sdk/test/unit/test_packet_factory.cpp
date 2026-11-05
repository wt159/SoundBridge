#include "../fixtures/RealMediaFixture.hpp"
#include <AudioBuffer.h>
#include <AudioCommon.hpp>
#include <ExtractorFactory.h>
#include <ExtractorHelper.hpp>
#include <FileSource.h>
#include <cstring>
#include <doctest/doctest.h>
#include <memory>
#include <string>
#include <vector>

// 包工厂的 expand 阶段（spec doc/streaming-playback-spec.md 第 3 节、ADR 0003、票面 #57）。
//
// 判据只有一条：**包是音频负载的唯一表示**，整块视图只是按需拼合的产物。
// 新增的单包迭代必须能遍历旧视图覆盖的同一段负载，边界逐项一致，且不复制包表；
// 旧形式（getMetaData() / getPacketizedMetaData()）同时保持可用，任何调用方都不用迁移。
//
// 本套件是「格式中立」的：只碰 ExtractorHelper 的包工厂面与量测仪器 AudioDecodeProcess，
// 不新增同步解码入口（#18 继续成立），也不改生产链 AudioStreamDecoder。

namespace {

// 包工厂替身：同时给出「逐包表」与「整块视图」两种旧形式，
// 用来锁住「游标按包走」和「没有包表时整块就是一包」两条契约。
class PacketTableExtractor : public ExtractorHelper {
public:
    explicit PacketTableExtractor(const std::vector<size_t> &packetSizes)
    {
        m_spec.sampleRate     = 44100;
        m_spec.numChannel     = 2;
        m_spec.bitsPerSample  = 16;
        m_spec.bytesPerSample = 2;
        m_spec.format         = AudioFormatS16;
        m_spec.durationMs     = 1000;

        for (size_t i = 0; i < packetSizes.size(); ++i) {
            AudioBuffer::AudioBufferPtr packet = std::make_shared<AudioBuffer>(packetSizes[i]);
            // 每包用不同字节填满，方便断言「取到的是哪一包」。
            std::memset(packet->data(), static_cast<int>(i + 1), packetSizes[i]);
            m_packets.push_back(packet);
        }
    }

    sdk_utils::status_t initCheck() override { return sdk_utils::OK; }
    AudioSpec getAudioSpec() override { return m_spec; }
    AudioCodecID getAudioCodecID() override { return AUDIO_CODEC_ID_NONE; }
    std::vector<AudioBuffer::AudioBufferPtr> getPacketizedMetaData() const override
    {
        return m_packets;
    }
    size_t packetCount() override { return m_packets.size(); }
    AudioBuffer::AudioBufferPtr packetAt(size_t index) override
    {
        return index < m_packets.size() ? m_packets[index] : nullptr;
    }

private:
    AudioSpec m_spec;
    std::vector<AudioBuffer::AudioBufferPtr> m_packets;
};

// 只持有整块负载的提取器（wav / mp3 / flac / ogg 这些现状就是这一个形状）。
class SingleBlockExtractor : public ExtractorHelper {
public:
    explicit SingleBlockExtractor(size_t bytes)
        : m_block(std::make_shared<AudioBuffer>(bytes))
    {
        m_spec.sampleRate     = 44100;
        m_spec.numChannel     = 2;
        m_spec.bitsPerSample  = 16;
        m_spec.bytesPerSample = 2;
        m_spec.format         = AudioFormatS16;
        m_spec.durationMs     = 1000;
        std::memset(m_block->data(), 0x5a, bytes);
    }

    sdk_utils::status_t initCheck() override { return sdk_utils::OK; }
    AudioSpec getAudioSpec() override { return m_spec; }
    AudioCodecID getAudioCodecID() override { return AUDIO_CODEC_ID_NONE; }
    AudioBuffer::AudioBufferPtr getMetaData() override { return m_block; }

private:
    AudioSpec m_spec;
    AudioBuffer::AudioBufferPtr m_block;
};

} // namespace

TEST_SUITE("PacketFactory")
{
    TEST_CASE("packet cursor walks a packetised payload in order and borrows the legacy packets")
    {
        PacketTableExtractor extractor({ 4, 8, 2 });
        const std::vector<AudioBuffer::AudioBufferPtr> legacy = extractor.getPacketizedMetaData();

        PacketCursor cursor(&extractor);
        std::vector<size_t> sizes;
        std::vector<AudioBuffer *> identities;
        AudioBuffer::AudioBufferPtr packet;
        while (cursor.nextPacket(packet)) {
            REQUIRE(packet != nullptr);
            sizes.push_back(packet->size());
            identities.push_back(packet.get());
        }

        CHECK(sizes == std::vector<size_t>({ 4, 8, 2 }));
        REQUIRE(identities.size() == legacy.size());
        for (size_t i = 0; i < identities.size(); ++i) {
            // 游标只借包不复制：逐包与旧逐包表是同一个对象（包表本身不新增常驻）。
            CHECK(identities[i] == legacy[i].get());
        }
        // 耗尽后继续拉不产出，也不重置。
        AudioBuffer::AudioBufferPtr afterEnd;
        CHECK(cursor.nextPacket(afterEnd) == false);
        CHECK(afterEnd == nullptr);
    }

    TEST_CASE("packet cursor treats a single-block payload as exactly one packet")
    {
        SingleBlockExtractor extractor(16);

        PacketCursor cursor(&extractor);
        AudioBuffer::AudioBufferPtr packet;
        REQUIRE(cursor.nextPacket(packet));
        CHECK(packet.get() == extractor.getMetaData().get());
        CHECK(packet->size() == 16);
        CHECK(cursor.nextPacket(packet) == false);
    }

    TEST_CASE("packet cursor yields nothing for an empty payload")
    {
        SingleBlockExtractor extractor(0);

        PacketCursor cursor(&extractor);
        AudioBuffer::AudioBufferPtr packet;
        CHECK(cursor.nextPacket(packet) == false);
        CHECK(packet == nullptr);
        CHECK(cursor.packetsProduced() == 0);
    }

    TEST_CASE("packet cursor can be rewound to replay the same sequence")
    {
        PacketTableExtractor extractor({ 3, 5 });
        PacketCursor cursor(&extractor);

        AudioBuffer::AudioBufferPtr packet;
        REQUIRE(cursor.nextPacket(packet));
        CHECK(packet->size() == 3);
        REQUIRE(cursor.nextPacket(packet));
        CHECK(packet->size() == 5);
        CHECK(cursor.packetsProduced() == 2);

        cursor.rewind();
        CHECK(cursor.packetsProduced() == 0);
        REQUIRE(cursor.nextPacket(packet));
        CHECK(packet->size() == 3);
    }

    TEST_CASE("real packetised file keeps legacy packet boundaries through the cursor")
    {
        RealMediaFixture fixture;
        const std::string name("music-ape.ape");
        if (!fixture.exists(name)) {
            MESSAGE("[SKIP] " << name << " (素材未随仓库下发)");
            return;
        }

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor;
        REQUIRE(fixture.create(name, ".ape", source, extractor));
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        const std::vector<AudioBuffer::AudioBufferPtr> legacy = extractor->getPacketizedMetaData();
        REQUIRE(legacy.size() > 1);

        PacketCursor cursor(extractor.get());
        size_t index = 0;
        std::vector<char> replayed;
        AudioBuffer::AudioBufferPtr packet;
        while (cursor.nextPacket(packet)) {
            REQUIRE(index < legacy.size());
            CHECK(packet->size() == legacy[index]->size());
            CHECK(packet.get() == legacy[index].get());
            replayed.insert(replayed.end(), packet->data(), packet->data() + packet->size());
            ++index;
        }
        CHECK(index == legacy.size());

        // 整块视图只是拼合产物：游标依次拼起来必须与它逐位一致。
        AudioBuffer::AudioBufferPtr joined = extractor->getMetaData();
        REQUIRE(joined != nullptr);
        REQUIRE(replayed.size() == joined->size());
        CHECK(std::memcmp(replayed.data(), joined->data(), joined->size()) == 0);
    }

    TEST_CASE("real single-block file exposes its whole payload as one packet")
    {
        RealMediaFixture fixture;
        const std::string name("像我这样的人-毛不易.128.mp3");
        if (!fixture.exists(name)) {
            MESSAGE("[SKIP] " << name << " (素材未随仓库下发)");
            return;
        }

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor;
        REQUIRE(fixture.create(name, ".mp3", source, extractor));
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        AudioBuffer::AudioBufferPtr joined = extractor->getMetaData();
        REQUIRE(joined != nullptr);
        CHECK(joined->size() > 0);

        PacketCursor cursor(extractor.get());
        AudioBuffer::AudioBufferPtr packet;
        REQUIRE(cursor.nextPacket(packet));
        CHECK(packet->size() == joined->size());
        CHECK(std::memcmp(packet->data(), joined->data(), joined->size()) == 0);
        CHECK(cursor.nextPacket(packet) == false);
    }
}
