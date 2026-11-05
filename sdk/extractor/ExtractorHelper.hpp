#pragma once

#include "AudioBuffer.h"
#include "AudioCommon.hpp"
#include "ByteUtils.h"
#include "DataSource.hpp"
#include "ErrorUtils.h"
#include <cstdint>
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

    // 观测点（规格第 5 节 / 票面 #56）：提取器 init 的只读扫描耗时，单位微秒。
    // 由 ExtractorFactory 在构造后回填（构造就是 init），解码器在 start() 时抄进 StreamMetrics。
    // 已知代价：当前 init 仍是「全量装载 payload」，所以这个数现在等于「启动开销」，
    // 不是纯粹的只读扫描；E2 把 init 改成只读扫描 + O(1) 摘要后，同一测量点自动变准。
    void setInitScanMicros(uint64_t micros) { m_initScanMicros = micros; }
    uint64_t initScanMicros() const { return m_initScanMicros; }

    // 分配记账（规格第 5 节 / ADR 0003 / 票面 #56）：此刻为当前曲目持有的音频数据字节数。
    // 必须计入 payload **与一切随包数增长的结构**（包索引 / 页表 / seek 表）——只算 payload
    // 会让常驻上界判据形同虚设。基类只报自己的拼接缓存，各提取器把自有的 payload
    // 与索引结构一起加上。固定容量对象（256 KB ring、AVIO 缓冲）不在这个口径里。
    virtual size_t heldPayloadBytes() const { return bufferBytes(m_joinedMetaBuf); }

    // 提取器**能确定**文件被截断时返回 true，与 AudioSpec 一样是事实上报、不是处置：
    // 典型是 wav/aiff「声明的数据长度 > 文件实际剩余」，短读在字节层面可判定，
    // 但这种量级的缺口（128 字节 ≈ 0.016%）低于 T4 的 5pp 阈值，不能靠时长比对发现。
    // 解码器在 EOS 汇总处据此报 DataTruncated（规格 3.2「能判定短读时」/ 条目 23）。
    virtual bool isDataTruncated() const { return false; }

protected:
    ExtractorHelper() { }

    static size_t bufferBytes(const AudioBuffer::AudioBufferPtr &buffer)
    {
        return buffer ? buffer->size() : 0;
    }

    // vector 的槽位本身也是随包数增长的结构（常数因子小，但不能漏）。
    template <typename T> static size_t vectorFootprint(const std::vector<T> &items)
    {
        return items.capacity() * sizeof(T);
    }

    // 包化的 payload：每包的字节 + 指针表的槽位。
    static size_t packetFootprint(const std::vector<AudioBuffer::AudioBufferPtr> &packets)
    {
        size_t total = vectorFootprint(packets);
        for (size_t i = 0; i < packets.size(); ++i) {
            total += bufferBytes(packets[i]);
        }
        return total;
    }

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
    // init 只读扫描耗时（观测点，见上）。
    uint64_t m_initScanMicros = 0;
};
