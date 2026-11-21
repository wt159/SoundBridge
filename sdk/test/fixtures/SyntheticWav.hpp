#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

/**
 * @brief 合成 WAV 素材：标准 44 字节 RIFF/WAVE 头 + PCM16 / 44.1 kHz / 双声道。
 * @details 判据要的是可控时长与可控负载，不依赖 `music/` 下的真实素材：
 *          - `makeSilentWavBytes`：全零数据，用于 EOS / 欠流 / 写失败判据；
 *          - `makeToneWavBytes`：非零方波，用于「只让全零负载失败」的静音分支判据
 *            （规格 §7.2），保证正常分支读出的 PCM 不可能整块为零。
 */
inline std::vector<char> makeWavBytes(int seconds, int16_t amplitude)
{
    const uint32_t sampleRate    = 44100;
    const uint16_t channels      = 2;
    const uint16_t bitsPerSample = 16;
    const uint32_t blockAlign    = channels * bitsPerSample / 8;
    const uint32_t dataSize
        = sampleRate * blockAlign * static_cast<uint32_t>(seconds < 1 ? 1 : seconds);
    std::vector<char> bytes(44 + dataSize, 0);

    auto putU16 = [&bytes](size_t offset, uint16_t value) {
        bytes[offset]     = static_cast<char>(value & 0xFF);
        bytes[offset + 1] = static_cast<char>((value >> 8) & 0xFF);
    };
    auto putU32 = [&bytes](size_t offset, uint32_t value) {
        for (int i = 0; i < 4; ++i) {
            bytes[offset + i] = static_cast<char>((value >> (8 * i)) & 0xFF);
        }
    };

    std::memcpy(&bytes[0], "RIFF", 4);
    putU32(4, 36 + dataSize);
    std::memcpy(&bytes[8], "WAVE", 4);
    std::memcpy(&bytes[12], "fmt ", 4);
    putU32(16, 16);
    putU16(20, 1); // WAVE_FORMAT_PCM
    putU16(22, channels);
    putU32(24, sampleRate);
    putU32(28, sampleRate * blockAlign);
    putU16(32, static_cast<uint16_t>(blockAlign));
    putU16(34, bitsPerSample);
    std::memcpy(&bytes[36], "data", 4);
    putU32(40, dataSize);

    if (amplitude != 0) {
        // 32 样本换向的方波：任意 4096 B 块都含非零样本，不会与「全零负载」判据混淆。
        const size_t samples = dataSize / blockAlign;
        for (size_t i = 0; i < samples; ++i) {
            const int16_t sample = ((i / 32) % 2 == 0) ? amplitude : static_cast<int16_t>(-amplitude);
            for (uint16_t ch = 0; ch < channels; ++ch) {
                const size_t offset = 44 + i * blockAlign + ch * 2;
                bytes[offset]       = static_cast<char>(sample & 0xFF);
                bytes[offset + 1]   = static_cast<char>((sample >> 8) & 0xFF);
            }
        }
    }
    return bytes;
}

inline std::vector<char> makeSilentWavBytes(int seconds) { return makeWavBytes(seconds, 0); }

inline std::vector<char> makeToneWavBytes(int seconds) { return makeWavBytes(seconds, 8000); }
