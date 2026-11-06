#pragma once
#include "AudioBuffer.h"
#include "AudioDecode.h"
#include "DataSource.hpp"
#include "ExtractorHelper.hpp"
#include "MP3FrameReader.h"
#include "NonCopyable.hpp"
#include <memory>
#include <vector>

/*
 *  MP3 file format
 *
 *  ID3v2
 *    +
 *  AudioFrame
 *   +
 *  ID3v1
 */

class MP3Extractor : public ExtractorHelper, public NonCopyable {
private:
    sdk_utils::status_t m_initCheck;
    DataSourceBase *m_dataSource;
    AudioCodecID m_audioCodecID;
    // 旧整块视图（测试量测仪器用）：只在被请求时按需读取，生产链不再持有整首 payload。
    AudioBuffer::AudioBufferPtr m_metaBuf;
    bool m_validFormat;
    AudioSpec m_spec;
    off64_t m_firstFramePos;
    uint32_t m_fixedHeader;
    int64_t m_dataSize;
    // 音频帧区间的末尾（去掉 ID3v1 尾标）：包序列不把尾标当坏数据交出去。
    int64_t m_audioEnd;

    // 包工厂（#62 / ADR 0003）：帧边界按 offset 现场算，init 只留 O(1) 摘要。
    std::unique_ptr<MP3FrameReader> m_reader;
    size_t m_nextPacketIndex;
    // 当前包序列的起点：新任务为音频起点，顺序重放 seek 也回到这里。
    int64_t m_sequenceOffset;

public:
    explicit MP3Extractor(DataSourceBase *source);
    static bool sniff(DataSourceBase *source);
    ~MP3Extractor() = default;
    virtual sdk_utils::status_t initCheck() { return m_initCheck; }
    virtual AudioSpec getAudioSpec() { return m_spec; }
    virtual AudioCodecID getAudioCodecID() { return m_audioCodecID; }
    virtual AudioBuffer::AudioBufferPtr getMetaData();
    // getDataSize()/getAudioDataOffset() 覆写 ExtractorHelper 的既有 off64_t 虚函数
    // （master 即有，不能改签名）；成员改用 int64_t 后，在既有边界处显式转回 off64_t。
    virtual off64_t getDataSize() { return static_cast<off64_t>(m_dataSize); }
    virtual off64_t getAudioDataOffset() { return static_cast<off64_t>(m_firstFramePos); }

    // —— 包工厂的单包面（#62 / ADR 0003）——
    // MPEG 帧自定界：每包一帧，总数事先未知（kUnknownPacketCount = 读到为止）。
    virtual size_t packetCount() { return ExtractorHelper::kUnknownPacketCount; }
    virtual AudioBuffer::AudioBufferPtr packetAt(size_t index);
    virtual bool seekPacketByte(int64_t offset);
    virtual int64_t packetBytePosition() const;

    // 分配记账：只报扫描缓冲（O(1)）。旧整块视图被仪器请求过时才多一份。
    virtual size_t heldPayloadBytes() const
    {
        return bufferBytes(m_metaBuf)
            + (m_reader != nullptr ? m_reader->heldBytes() : 0);
    }

private:
    sdk_utils::status_t init();
    // 包工厂在播放期第一次拉包时才创建（demux 句柄由解码侧持有，规格 4.3）。
    MP3FrameReader *reader();
};
