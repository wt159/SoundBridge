#include "../fixtures/RealMediaFixture.hpp"
#include <AudioBuffer.h>
#include <AudioCommon.hpp>
#include <ExtractorFactory.h>
#include <ExtractorHelper.hpp>
#include <FileSource.h>
#include <algorithm>
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
// 本套件只碰 ExtractorHelper 的包工厂面：不新增同步解码入口（#18 继续成立），
// 也不改生产链 AudioStreamDecoder。

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

    // FLAC 的增量包面（票面 #60 / 规格第 9 节 E7）。
    //
    // FLAC 没有 ffmpeg demux，帧边界只能按 offset 现场重算（ADR 0003「读过 ≠ 常驻」）。
    // 契约形状：首包是流头（fLaC + 元数据块，libFLAC 要有 STREAMINFO 才做整段 MD5 校验），
    // 之后每包一帧；init 期不留任何随包数增长的结构。
    TEST_CASE("flac packet factory yields frame boundaries without holding the stream")
    {
        RealMediaFixture fixture;
        const std::string name("小镇姑娘-陶喆.flac");
        if (!fixture.exists(name)) {
            MESSAGE("[SKIP] " << name << " (素材未随仓库下发)");
            return;
        }

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor;
        REQUIRE(fixture.create(name, ".flac", source, extractor));
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        // init 只留 O(1) 定长摘要：构造完成时提取器不持有任何 payload 或索引。
        CHECK(extractor->heldPayloadBytes() == 0);
        // 未知总数用哨兵表示；0 的含义是「没有包」，那会让解码侧退回全量形式。
        CHECK(extractor->packetCount() != 0);

        PacketCursor cursor(extractor.get());
        AudioBuffer::AudioBufferPtr packet;
        size_t count                = 0;
        size_t totalBytes           = 0;
        size_t maxPacketBytes       = 0;
        size_t heldAfterMuchLater   = 0;
        size_t heldAtEnd            = 0;
        bool contiguous             = true;
        bool allFramesStartWithSync = true;
        bool firstIsStreamHeader    = false;
        off64_t nextOffset          = 0;

        while (cursor.nextPacket(packet)) {
            REQUIRE(packet != nullptr);
            REQUIRE(packet->size() > 0);
            const off64_t packetStart = nextOffset;
            ++count;
            totalBytes += packet->size();
            maxPacketBytes = std::max(maxPacketBytes, packet->size());

            // 包首尾相接、覆盖整段负载：本包从上一包的结束处开始，位置只增不减。
            if (nextOffset + static_cast<off64_t>(packet->size())
                != extractor->packetBytePosition()) {
                contiguous = false;
            }
            nextOffset = extractor->packetBytePosition();

            if (count == 1) {
                firstIsStreamHeader = packet->size() >= 4
                    && std::memcmp(packet->data(), "fLaC", 4) == 0;
            }
            // 流头之后每包一帧：起点在音频数据区里的包必须以帧同步码开头。
            // （流头本身按 64 KB 切片，大封面不会把峰值抬起来。）
            if (packetStart >= extractor->getAudioDataOffset()) {
                const unsigned char *head = reinterpret_cast<const unsigned char *>(packet->data());
                const bool sync           = head[0] == 0xFF && (head[1] & 0xFE) == 0xF8;
                allFramesStartWithSync = allFramesStartWithSync && sync;
            }
            if (count == 1000) {
                heldAfterMuchLater = extractor->heldPayloadBytes();
            }
        }
        heldAtEnd = extractor->heldPayloadBytes();

        CHECK(firstIsStreamHeader);
        CHECK(allFramesStartWithSync);
        CHECK(contiguous);
        CHECK(static_cast<off64_t>(totalBytes) == extractor->getDataSize());
        // 一包一帧：297 s / 4608 采样的块大小 ≈ 2850 帧，绝不是「整首一包」。
        CHECK(count > 1000);
        // 单帧压缩后远小于整首；上界只用来防「整首一包」式的退化。
        CHECK(maxPacketBytes < 64 * 1024);
        // 持有量不随包数增长：第 1000 包之后与序列末尾的读数必须相同。
        CHECK(heldAfterMuchLater == heldAtEnd);
        CHECK(heldAtEnd <= maxPacketBytes * 2 + 16 * 1024);

        MESSAGE(name << ": packets " << count << ", totalBytes " << totalBytes
                     << ", maxPacketBytes " << maxPacketBytes << ", held "
                     << heldAfterMuchLater);
    }

    // mkv 的增量包面（票面 #63 / 规格第 9 节 E10）。
    //
    // mkv 的块边界只有解复用器知道（变长块 + 交错 cluster，字节长度无法从采样数反推），
    // 所以播放期重开一次 demux、按 offset 顺序读块：每拉一包，边界由 demux 在现场解析
    // 经过的 cluster / block 头得到，不保留逐包时间轴、不建 seek 表。
    // init 期只做一次只读扫描取标称时长，留下的全是 O(1) 定长摘要。
    TEST_CASE("mkv packet factory recomputes block boundaries without holding the container")
    {
        RealMediaFixture fixture;
        const std::string name("music-mkv.mkv");
        if (!fixture.exists(name)) {
            MESSAGE("[SKIP] " << name << " (素材未随仓库下发)");
            return;
        }

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor;
        REQUIRE(fixture.create(name, ".mkv", source, extractor));
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        // init 只读扫描：读完全文取标称时长，但不留逐包表、不建索引（读过 ≠ 常驻）。
        CHECK(extractor->heldPayloadBytes() == 0);
        // 未知总数用哨兵表示：0 的含义是「没有包」，那会让解码侧退回全量形式。
        CHECK(extractor->packetCount() == ExtractorHelper::kUnknownPacketCount);
        // 标称时长仍按包时间轴累加。容器声明字段写错（music-mkv.mkv 把 393216 个采样
        // 当毫秒写成 393.216 s）时不能改口径：24.576 s 与覆盖度基线一致。
        CHECK(extractor->getAudioSpec().durationMs == 24576);

        PacketCursor cursor(extractor.get());
        AudioBuffer::AudioBufferPtr packet;
        size_t count             = 0;
        size_t totalBytes        = 0;
        size_t maxPacketBytes    = 0;
        size_t heldAfterMuchLater = 0;
        size_t heldAtEnd         = 0;
        off64_t lastPosition     = 0;
        bool monotonicPosition   = true;

        while (cursor.nextPacket(packet)) {
            REQUIRE(packet != nullptr);
            REQUIRE(packet->size() > 0);
            ++count;
            totalBytes += packet->size();
            maxPacketBytes = std::max(maxPacketBytes, packet->size());

            // 边界在播放期按 offset 推进：位置只增不减，且不回读已交出的块。
            const off64_t position = extractor->packetBytePosition();
            if (position < lastPosition) {
                monotonicPosition = false;
            }
            lastPosition = position;

            if (count == 10) {
                heldAfterMuchLater = extractor->heldPayloadBytes();
            }
        }
        heldAtEnd = extractor->heldPayloadBytes();

        // 一块一包：24.5 s 的素材不止十几个块，绝不是「整首一包」。
        CHECK(count > 10);
        CHECK(totalBytes > 0);
        // 单块远小于整份 payload（789 KB）；上界只用来防整首一包的退化。
        CHECK(maxPacketBytes < 64 * 1024);
        CHECK(monotonicPosition);
        // 持有量不随包数增长：第 10 包之后与序列末尾的读数必须相同。
        CHECK(heldAfterMuchLater == heldAtEnd);
        // 常驻只有 AVIO 缓冲（判据公式里的独立项），提取器自己不记 payload。
        CHECK(heldAtEnd == 0);

        // 二次迭代：顺序重放 seek（#49 第二档）依赖「游标回起点后能再走一遍」。
        cursor.rewind();
        size_t secondCount = 0;
        size_t secondBytes = 0;
        while (cursor.nextPacket(packet)) {
            ++secondCount;
            secondBytes += packet->size();
        }
        CHECK(secondCount == count);
        CHECK(secondBytes == totalBytes);

        MESSAGE(name << ": packets " << count << ", totalBytes " << totalBytes
                     << ", maxPacketBytes " << maxPacketBytes << ", held " << heldAtEnd);
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
