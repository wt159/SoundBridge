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
// payload 的契约（来源 #17、#47 / ADR 0003）：**包是唯一层间单元**。
//   - packetCount() / packetAt() 加 PacketCursor 是包工厂的单包面，解码侧逐包 pull、零复制；
//     11 种格式全部 override，init 期只读扫描只留 O(1) 摘要、播放期按 offset 现场重算包边界。
//   - getMetaData() / getPacketizedMetaData() 的整块 / 全量 vector 形式已退出生产链（#66 contract）；
//     接口按票面保留测试专用：合成提取器 override getMetaData() 交出假 PCM，量测仪器可用整块视图。
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

    // —— 包工厂的单包面（#57 / #66 / ADR 0003）——
    // 解码侧按需 pull 单包；两个方法必须 O(1)：不允许为了「交出第 i 包」复制包表、
    // 拼合负载或重建索引。11 种格式全部 override packetCount() / packetAt()，
    // 包边界在播放期现场重算（#60 起）。基类默认实现把 getMetaData() 的整块视图
    // 当作一整包，只服务测试里的合成提取器（它们不 override 这两个方法）。
    //
    // 未知总数（#60 起）：包边界在播放期按 offset 现场重算的格式（FLAC 的帧扫描、
    // wav / aiff 的定长切片）事先不知道有多少包。这类提取器让 packetCount() 返回
    // kUnknownPacketCount，序列以 packetAt() 返回 nullptr 结束——「读完为止」，
    // 不建任何随包数增长的索引。
    static const size_t kUnknownPacketCount = static_cast<size_t>(-1);

    virtual size_t packetCount() { return getMetaData() == nullptr ? 0 : 1; }
    virtual AudioBuffer::AudioBufferPtr packetAt(size_t index)
    {
        if (index != 0) {
            return nullptr;
        }
        return getMetaData();
    }

    // 单包序列的字节定位（FLAC 的 native random access 用，ADR 0003 / #49）：
    // 提取器把「下一包从哪个文件偏移开始」作为 O(1) 状态暴露给解码侧，
    // 解码器只负责把 libFLAC 的 seek/tell 转成这两个调用，不自己算包边界。
    virtual bool seekPacketByte(int64_t offset)
    {
        (void)offset;
        return false;
    }
    virtual int64_t packetBytePosition() const { return 0; }

    // 整块视图（测试专用）：合成提取器 override 它交出假 PCM，量测仪器用它拼整段。
    // 默认实现把包拼起来并缓存：没有包化数据时返回 nullptr，与「没有 payload」同义。
    // 生产链零调用（#66 contract）；播放器不为当前曲目持有整首压缩 payload。
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
    // #56–#65 后 init 已是只读扫描 + O(1) 摘要，全量装载 payload 不再发生；
    // 这个数 = 启动开销（含只读扫描），按规格不进判据、不设阈值。
    void setInitScanMicros(uint64_t micros) { m_initScanMicros = micros; }
    uint64_t initScanMicros() const { return m_initScanMicros; }

    // 分配记账（规格第 5 节 / ADR 0003 / 票面 #56）：此刻为当前曲目持有的音频数据字节数。
    // 必须计入 payload **与一切随包数增长的结构**（包索引 / 页表 / seek 表）——只算 payload
    // 会让常驻上界判据形同虚设。基类只报自己的拼接缓存，各提取器把自有的 payload
    // 与索引结构一起加上。固定容量对象（256 KB ring、AVIO 缓冲）不在这个口径里。
    virtual size_t heldPayloadBytes() const { return bufferBytes(m_joinedMetaBuf); }

    // 现场重算包边界的 CPU 成本（微秒，规格第 5 节末段）：只记独立指标、不设阈值，
    // 也不参与任何判据的接受 / 拒绝。只有包边界在播放期按 offset 重算的格式会记账
    // （#63 起的 mkv；m4a / ape / asf 归 #64），其余格式恒为 0。
    virtual uint64_t packetBoundaryMicros() const { return 0; }

    // 提取器**能确定**文件被截断时返回 true，与 AudioSpec 一样是事实上报、不是处置：
    // 典型是 wav/aiff「声明的数据长度 > 文件实际剩余」，短读在字节层面可判定，
    // 但这种量级的缺口（128 字节 ≈ 0.016%）低于 T4 的 5pp 阈值，不能靠时长比对发现。
    // 解码器在 EOS 汇总处据此报 DataTruncated（规格 3.2「能判定短读时」/ 条目 23）。
    virtual bool isDataTruncated() const { return false; }

    // —— P3 直通源生命周期接口（doc/live-source-spec.md §5/§6，ADR 0006 / 0007）——
    // 三者都是 sdk 内部面（ExtractorHelper 本就不在公开 API 里），默认 no-op：
    // 本地提取器与 HttpSource 直读路径不经此口，解码侧按 false 分流回既有路径。
    //
    // 诊断旁路（#151 / ADR 0006）：返回最近一次 av_read_frame 负返回并**消费一次失效**。
    // 不是第二个层间单元——包仍是唯一层间单元，packetAt 的 nullptr 语义不扩展；
    // 三条生命周期不变量：open 置无失败 / 任一成功读包即清零 / 消费一次失效。
    // 返回 false 表示本提取器无诊断（调用方按既有 EOS 汇总处置）。
    virtual bool consumeLastReadError(int *outError)
    {
        if (outError != nullptr) *outError = 0;
        return false;
    }

    // 无状态重开原语（#149 / ADR 0007）：closeFmt + open，从新滑动窗口头顺序迭代。
    // 不持预算、不 sleep、不重试、不判可解性——预算与等待在解码线程状态机里。
    // 返回 false = 本提取器不支持重开或本次 open 失败（调用方计入重连预算）。
    virtual bool reopenConnection() { return false; }

    // 直通 / 无限流形态支持性（与重开能力同源同一提取器）：seek 归零例外在无限流上
    // 静默短路的判据（#153 D21），也是解码侧「单一入口」进重连通道的前提。
    // sdk 内部判据，不是 player.h 公开能力位（#75 §7 不引入 isSeekable()）。
    virtual bool supportsLiveReconnect() const { return false; }

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
    // 未知总数（kUnknownPacketCount）的序列没有「跳过空包继续」的余地：nullptr 就是结束。
    bool nextPacket(AudioBuffer::AudioBufferPtr &packet)
    {
        packet.reset();
        if (m_extractor == nullptr) {
            return false;
        }

        const size_t count    = m_extractor->packetCount();
        const bool countKnown = count != ExtractorHelper::kUnknownPacketCount;
        while (!countKnown || m_nextIndex < count) {
            AudioBuffer::AudioBufferPtr current = m_extractor->packetAt(m_nextIndex);
            ++m_nextIndex;
            if (current == nullptr) {
                if (!countKnown) {
                    return false;
                }
                continue;
            }
            if (current->size() > 0) {
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
