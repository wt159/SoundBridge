#pragma once

#include "AudioBuffer.h"
#include "AudioCommon.hpp"
#include "ByteUtils.h"
#include "DataSource.hpp"
#include "ErrorUtils.h"
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

enum standardExtractors {
    WAV_EXTRACTOR,
    MP3_EXTRACTOR,
    AAC_EXTRACTOR,
    FLAC_EXTRACTOR,
    OGG_EXTRACTOR,
    AIFF_EXTRACTOR,
    ASF_EXTRACTOR,
    M4A_EXTRACTOR,
    APE_EXTRACTOR,
    MKV_EXTRACTOR,
    UNKNOWN_EXTRACTOR
};

// 受支持格式的唯一真源：扫描（FileSearch）与断言都以它为机器可读清单，
// 人读版本见 doc/format-support.md，二者由 test_extractor.cpp 的一致性用例锁定。
// 不要把未实现解码路径的扩展名写进来——.amr 与 .opus 曾在此处虚报支持。
static const std::unordered_map<std::string, standardExtractors> defaultExtractorMap = {
    { ".wav", WAV_EXTRACTOR },
    { ".aac", AAC_EXTRACTOR },
    { ".mp3", MP3_EXTRACTOR },
    { ".flac", FLAC_EXTRACTOR },
    { ".m4a", M4A_EXTRACTOR },
    { ".ogg", OGG_EXTRACTOR },
    { ".aiff", AIFF_EXTRACTOR },
    { ".asf", ASF_EXTRACTOR },
    { ".ape", APE_EXTRACTOR },
    { ".mkv", MKV_EXTRACTOR },
    { ".wma", ASF_EXTRACTOR }
};

// 提取器只上报「容器里能确定的事实」。AudioSpec 的契约（来源 #21）：
//   - sampleRate / numChannel：必须有效（> 0）。ExtractorFactory::createExtractor 在出口校验，
//     不合格直接返回 nullptr；
//   - format / bytesPerSample：允许 Unknown / 0。mp3、ogg 这类容器不声明 PCM 采样格式，
//     真实值要等解码首帧才知道，调用方必须按首帧回填（AudioStreamDecoder 的懒重采样、
//     AudioDecodeProcess 的回调都这么做）。
// 任何「拿 spec 直接算字节数或时长」的代码，都必须先处理第二类字段为 0 的情况。
//
// payload 的契约（来源 #17）：**包是唯一表示**。
//   - getPacketizedMetaData() 返回解复用器给出的原始包边界；
//   - getMetaData() 是「把包拼成一整块」的便捷视图，基类按需拼合并缓存，
//     提取器不要自己再存一份拼接副本（历史上 APE/ASF 因此各存了两份，等于 2× 文件大小）；
//   - 为什么必须由提取器给边界：ffmpeg 的 parser 只覆盖部分 codec（aac/mp3/flac/vorbis/opus 有，
//     ape/wmav2/alac/pcm 没有）。没有 parser 时解码器「一个 packet 出有限帧」，
//     把整段音频当成一个 packet 送进去只能解出开头——APE 曾因此 4.6s/24.9s。
//     边界只有解复用器知道（变长帧的字节长度无法从采样数反推），所以解码器侧兜底做不到。
class ExtractorHelper {
public:
    virtual ~ExtractorHelper() { }

    virtual const char *name() { return "<unspecified>"; }
    virtual sdk_utils::status_t initCheck()           = 0;
    virtual AudioSpec getAudioSpec()                  = 0;
    virtual AudioCodecID getAudioCodecID()            = 0;
    virtual std::vector<AudioBuffer::AudioBufferPtr> getPacketizedMetaData() const
    {
        return std::vector<AudioBuffer::AudioBufferPtr>();
    }

    // —— 包工厂的单包面（expand 阶段，#57 / ADR 0003）——
    // 解码侧按需 pull 单包，整块视图只是按需拼合的产物；两个方法必须 O(1)：
    // 不允许为了「交出第 i 包」复制包表、拼合负载或重建索引。
    // 默认实现把**按需拼合的整块视图**当作一整包：wav / mp3 / flac / ogg / aiff / aac
    // 这些提取器本来就只持有整块 payload，边界就是整块本身，与旧路径 decode(data, size)
    // 完全一致。持有逐包表的提取器（ape / asf / m4a / mkv）覆写这两个方法，零复制交出旧边界，
    // 调用方因此不需要迁移任何东西。
    virtual size_t packetCount() { return getMetaData() == nullptr ? 0 : 1; }
    virtual AudioBuffer::AudioBufferPtr packetAt(size_t index)
    {
        if (index != 0) {
            return nullptr;
        }
        return getMetaData();
    }

    // 需要连续缓冲的消费者（FLAC/Vorbis 专用解码路径、部分用例）走这里。
    // 默认实现把包拼起来并缓存：没有包化数据时返回 nullptr，与「没有 payload」同义。
    virtual AudioBuffer::AudioBufferPtr getMetaData()
    {
        if (m_joinedMetaBuf) {
            return m_joinedMetaBuf;
        }

        const std::vector<AudioBuffer::AudioBufferPtr> packets = getPacketizedMetaData();
        size_t total                                           = 0;
        for (size_t i = 0; i < packets.size(); ++i) {
            if (packets[i]) {
                total += packets[i]->size();
            }
        }
        if (total == 0) {
            return nullptr;
        }

        m_joinedMetaBuf = std::make_shared<AudioBuffer>(total);
        size_t offset   = 0;
        for (size_t i = 0; i < packets.size(); ++i) {
            if (!packets[i] || packets[i]->size() == 0) {
                continue;
            }
            memcpy(m_joinedMetaBuf->data() + offset, packets[i]->data(), packets[i]->size());
            offset += packets[i]->size();
        }
        return m_joinedMetaBuf;
    }

    virtual AudioBuffer::AudioBufferPtr getCodecExtraData() { return nullptr; }
    virtual int getBitRate() { return 0; }
    virtual int getBlockAlign() { return 0; }
    virtual sdk_utils::status_t seekToMs(uint64_t targetMs, uint64_t *actualMs = nullptr)
    {
        (void)actualMs;
        (void)targetMs;
        return sdk_utils::INVALID_OPERATION;
    }
    virtual DataSourceBase *getDataSource() { return nullptr; }
    virtual off64_t getDataSize() { return 0; }
    virtual off64_t getAudioDataOffset() { return 0; }

    // 提取器**能确定**文件被截断时返回 true，与 AudioSpec 一样是事实上报、不是处置：
    // 典型是 wav/aiff「声明的数据长度 > 文件实际剩余」，短读在字节层面可判定，
    // 但这种量级的缺口（128 字节 ≈ 0.016%）低于 T4 的 5pp 阈值，不能靠时长比对发现。
    // 解码器在 EOS 汇总处据此报 DataTruncated（规格 3.2「能判定短读时」/ 条目 23）。
    virtual bool isDataTruncated() const { return false; }

protected:
    ExtractorHelper() { }

    static void normalizeAudioSpec(AudioSpec &spec)
    {
        if (spec.bitsPerSample > 0 && spec.bytesPerSample == 0) {
            spec.bytesPerSample = (spec.bitsPerSample + 7) / 8;
        }
    }

private:
    ExtractorHelper(const ExtractorHelper &);
    ExtractorHelper &operator=(const ExtractorHelper &);

    // getMetaData() 的拼接缓存：只有被请求过才分配，提取器自己不保存副本。
    AudioBuffer::AudioBufferPtr m_joinedMetaBuf;
};

// 单包迭代游标（expand 阶段，#57 / ADR 0003）。包是层间唯一单元，
// 解码线程按需 pull 下一包，不主动推。
//
// 只持有「下一包序号」与提取器指针，是 O(1) 常驻：不复制包表、不缓存负载、
// 不预读、不建索引，所以不改变 heldPayloadBytes() 的口径，也不需要额外的分配记账。
// 生命周期由拉取方（解码线程 / 量测仪器）持有；游标存活期间提取器必须存活。
// 有且只有 forward + rewind 两种移动：随机定位属于 seek 策略（#49），不在这里开口子。
class PacketCursor {
public:
    explicit PacketCursor(ExtractorHelper *extractor)
        : m_extractor(extractor)
        , m_nextIndex(0)
        , m_produced(0)
    {
    }

    // 拉下一包；返回 false 表示序列正常结束。空包（nullptr 或 0 字节）不产出、直接跳过，
    // 与整块视图拼合时「跳过空包」的语义一致。
    bool nextPacket(AudioBuffer::AudioBufferPtr &packet)
    {
        packet.reset();
        if (m_extractor == nullptr) {
            return false;
        }

        const size_t count = m_extractor->packetCount();
        while (m_nextIndex < count) {
            AudioBuffer::AudioBufferPtr current = m_extractor->packetAt(m_nextIndex);
            ++m_nextIndex;
            if (current != nullptr && current->size() > 0) {
                packet = current;
                ++m_produced;
                return true;
            }
        }
        return false;
    }

    // 回到序列起点（供量测仪器二次遍历用；生产链的顺序重放同样从这里起）。
    void rewind()
    {
        m_nextIndex = 0;
        m_produced  = 0;
    }

    size_t packetsProduced() const { return m_produced; }

private:
    ExtractorHelper *m_extractor;
    size_t m_nextIndex;
    size_t m_produced;
};
