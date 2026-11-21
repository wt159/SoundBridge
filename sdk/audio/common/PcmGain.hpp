#pragma once

#include "AudioCommon.hpp"
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace sdk {

// 软件增益（ADR 0013）：对一段**交错** PCM 施加线性渐变的缩放。
//
// fromGain → toGain 在整段内线性过渡，用来消除块边界上的增益阶跃：消费拍每块
// 4096 B（S16/44.1k/2ch ≈ 23 ms），直接跳变会产生可听的 zipper 噪声。
// 渐变让「静音 / 恢复 / 拖动滑杆」在约一块的时间内平滑过去。
//
// 约定：
// - 只处理本仓库实际会产出的有符号整数与 float 格式；未识别格式返回 false 且
//   **不改动任何字节**——宁可音量失效，也不要把字节解释错。
// - 调用方在 from == to == 1.0 时应整段跳过，本函数不做这个判断（保持纯函数）。
// - 越界一律钳制：整数按类型端点，float 按 [-1, 1]。
inline bool applyGainRamp(char *data, size_t bytes, AudioFormat format, float fromGain,
                          float toGain)
{
    if (data == nullptr || bytes == 0) {
        return true;
    }

    switch (format) {
    case AudioFormatS16: {
        // 只处理本机字节序的变体；*BE 变体在数据层就要求换序，这里不做（见 default）。
        int16_t *samples    = reinterpret_cast<int16_t *>(data);
        const size_t count  = bytes / sizeof(int16_t);
        float gain          = fromGain;
        const float step    = (count > 1) ? (toGain - fromGain) / static_cast<float>(count - 1)
                                          : 0.0f;
        for (size_t i = 0; i < count; ++i) {
            const double scaled = static_cast<double>(samples[i]) * static_cast<double>(gain);
            double clamped      = std::floor(scaled + 0.5);
            if (clamped > 32767.0) {
                clamped = 32767.0;
            }
            if (clamped < -32768.0) {
                clamped = -32768.0;
            }
            samples[i] = static_cast<int16_t>(clamped);
            gain += step;
        }
        return true;
    }
    case AudioFormatS32: {
        int32_t *samples   = reinterpret_cast<int32_t *>(data);
        const size_t count = bytes / sizeof(int32_t);
        float gain         = fromGain;
        const float step   = (count > 1) ? (toGain - fromGain) / static_cast<float>(count - 1)
                                         : 0.0f;
        for (size_t i = 0; i < count; ++i) {
            const double scaled = static_cast<double>(samples[i]) * static_cast<double>(gain);
            double clamped      = std::floor(scaled + 0.5);
            if (clamped > 2147483647.0) {
                clamped = 2147483647.0;
            }
            if (clamped < -2147483648.0) {
                clamped = -2147483648.0;
            }
            samples[i] = static_cast<int32_t>(clamped);
            gain += step;
        }
        return true;
    }
    case AudioFormatFLT32: {
        float *samples     = reinterpret_cast<float *>(data);
        const size_t count = bytes / sizeof(float);
        float gain         = fromGain;
        const float step   = (count > 1) ? (toGain - fromGain) / static_cast<float>(count - 1)
                                         : 0.0f;
        for (size_t i = 0; i < count; ++i) {
            float value = samples[i] * gain;
            if (value > 1.0f) {
                value = 1.0f;
            }
            if (value < -1.0f) {
                value = -1.0f;
            }
            samples[i] = value;
            gain += step;
        }
        return true;
    }
    case AudioFormatU8: {
        // 无符号 8 位以 128 为零点：缩放必须围绕零点做，否则静音会变成半个直流偏置。
        uint8_t *samples   = reinterpret_cast<uint8_t *>(data);
        const size_t count = bytes;
        float gain         = fromGain;
        const float step   = (count > 1) ? (toGain - fromGain) / static_cast<float>(count - 1)
                                         : 0.0f;
        for (size_t i = 0; i < count; ++i) {
            double value = 128.0 + (static_cast<double>(samples[i]) - 128.0)
                                       * static_cast<double>(gain);
            if (value > 255.0) {
                value = 255.0;
            }
            if (value < 0.0) {
                value = 0.0;
            }
            samples[i] = static_cast<uint8_t>(std::floor(value + 0.5));
            gain += step;
        }
        return true;
    }
    case AudioFormatS8: {
        int8_t *samples    = reinterpret_cast<int8_t *>(data);
        const size_t count = bytes;
        float gain         = fromGain;
        const float step   = (count > 1) ? (toGain - fromGain) / static_cast<float>(count - 1)
                                         : 0.0f;
        for (size_t i = 0; i < count; ++i) {
            double value = static_cast<double>(samples[i]) * static_cast<double>(gain);
            if (value > 127.0) {
                value = 127.0;
            }
            if (value < -128.0) {
                value = -128.0;
            }
            samples[i] = static_cast<int8_t>(std::floor(value + 0.5));
            gain += step;
        }
        return true;
    }
    default:
        // 16/24 位无符号、24 位有符号、double 与大小端变体都不在增益路径上：
        // 直通不改，由 setVolume 的调用方（app）看到「音量没生效」而不是听到噪声。
        return false;
    }
}

} // namespace sdk
