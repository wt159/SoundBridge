#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>

class AudioRingBuffer;

namespace sdk_utils {

// 首播门槛的等待原语（规格 doc/buffering-switching-spec.md §2）。
// 语义：轮询 ring，直到可读量 >= targetBytes（Reached）、超时（TimedOut）
// 或调用方给出的取消条件成立（Aborted）。1 ms 粒度与解码线程的背压同款，
// 不引入条件变量、不给 ring 加 mutex。
enum class PrefillResult {
    Reached,
    TimedOut,
    Aborted,
};

PrefillResult waitForPrefill(const AudioRingBuffer &ring, size_t targetBytes, uint32_t timeoutMs,
                             const std::function<bool()> &aborted);
}
