#pragma once

#include "AudioBuffer.h"
#include "DataSource.hpp"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>

// 原始 PCM 的包工厂（票面 #61 / ADR 0003：包是层间唯一单元、读过 ≠ 常驻）。
//
// wav / aiff 的负载是没有帧头的线性 PCM，没有「解复用器给的边界」可言，所以边界只能是
// **定长切片**，长度对齐到帧（numChannel × bytesPerSample）：每包都是整数帧，
// 逐包拼接与旧的整块视图逐位一致。切片长度是固定常数，与素材时长无关——常驻上界里的 P
// 由它决定；索引 / 时间轴产物为零（O(1) 状态 = 一个文件偏移 + 一段长度）。
//
// 交出之后的数据归调用方持有：本类不留读缓冲，heldBytes() 恒为 0，
// 「为当前曲目持有多少音频数据」因此只在解码侧的包记账里出现一次。
class PcmPacketReader {
public:
    // 单包字节上限。与 ring（256 KB）同量级以下的固定值：一次写 ring 不会被单包撑爆背压，
    // 坏数据也不会为了「读出下一包」把整首拉进内存。
    static const size_t kPacketBytes = 64 * 1024;

    PcmPacketReader(DataSourceBase *source, off64_t dataOffset, off64_t dataSize,
                    size_t frameBytes)
        : m_source(source)
        , m_dataOffset(dataOffset)
        , m_dataSize(dataSize > 0 ? dataSize : 0)
        , m_nextOffset(0)
        , m_packetBytes(alignPacketBytes(frameBytes))
    {
    }

    // 回到负载起点：新任务与顺序重放 seek 都从这里重新按包迭代。
    void rewind() { m_nextOffset = 0; }

    // 下一包从哪个文件偏移开始（绝对偏移，与 FLAC 的 packetBytePosition 同口径）。
    off64_t position() const { return m_dataOffset + m_nextOffset; }

    // 拉下一包；返回 false 表示序列正常结束。
    bool nextPacket(AudioBuffer::AudioBufferPtr &packet)
    {
        packet.reset();
        if (m_source == nullptr || m_nextOffset >= m_dataSize) {
            return false;
        }

        const size_t remaining = static_cast<size_t>(m_dataSize - m_nextOffset);
        const size_t want      = std::min(m_packetBytes, remaining);

        AudioBuffer::AudioBufferPtr block = std::make_shared<AudioBuffer>(want);
        const ssize_t got = m_source->readAt(m_dataOffset + m_nextOffset, block->data(), want);
        if (got <= 0) {
            // 读不动了就是序列结束：可解性不在这里判（ADR 0002，EOS 汇总兜底）。
            return false;
        }
        if (static_cast<size_t>(got) != want) {
            // 短读（init 之后文件变短）：只交出真正读到的那些字节，下一次从该处继续。
            AudioBuffer::AudioBufferPtr tail
                = std::make_shared<AudioBuffer>(static_cast<size_t>(got));
            std::memcpy(tail->data(), block->data(), static_cast<size_t>(got));
            block = tail;
        }
        m_nextOffset += static_cast<off64_t>(got);
        packet = block;
        return true;
    }

private:
    // 切片长度对齐到整帧：不对齐会在包边界上把一个采样劈成两半。
    static size_t alignPacketBytes(size_t frameBytes)
    {
        if (frameBytes == 0) {
            return kPacketBytes;
        }
        if (frameBytes >= kPacketBytes) {
            return frameBytes;
        }
        return (kPacketBytes / frameBytes) * frameBytes;
    }

    DataSourceBase *m_source;
    off64_t m_dataOffset;
    off64_t m_dataSize;
    off64_t m_nextOffset;
    size_t m_packetBytes;
};
