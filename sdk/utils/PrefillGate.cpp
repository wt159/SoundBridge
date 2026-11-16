#include "PrefillGate.h"
#include "AudioRingBuffer.h"
#include <chrono>
#include <thread>

namespace sdk_utils {

PrefillResult waitForPrefill(const AudioRingBuffer &ring, size_t targetBytes, uint32_t timeoutMs,
                             const std::function<bool()> &aborted)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        if (ring.availableRead() >= targetBytes) {
            return PrefillResult::Reached;
        }
        if (aborted && aborted()) {
            return PrefillResult::Aborted;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return PrefillResult::TimedOut;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
}
