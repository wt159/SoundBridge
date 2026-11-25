#include "ScanExecutor.h"

namespace sdk {

ScanExecutor::ScanExecutor()
    : m_shared(std::make_shared<Shared>())
{
    std::shared_ptr<Shared> shared = m_shared;
    m_thread                      = std::thread([shared]() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(shared->mutex);
                shared->wake.wait(lock, [&shared]() { return shared->stopping || shared->hasPending; });
                // 停止优先于待执行作业：调用方在 shutdown 前已把请求置为 cancelled，
                // 未开始的作业再跑一遍只会白读盘。
                if (shared->stopping) {
                    shared->exited = true;
                    shared->idle.notify_all();
                    return;
                }
                job = std::move(shared->pending);
                shared->pending = Job();
                shared->hasPending = false;
            }
            if (job) {
                job();
            }
        }
    });
}

ScanExecutor::~ScanExecutor()
{
    shutdown(std::chrono::milliseconds(200));
}

void ScanExecutor::submit(const Job &job)
{
    std::lock_guard<std::mutex> lock(m_shared->mutex);
    if (m_shared->stopping) {
        return;
    }
    // 深度为 1：尚未开始的旧作业被替换。正在执行的那一份不在队列里，由调用方
    // 通过请求的 cancelled 位作废（ADR 0021 决策 6）。
    m_shared->pending    = job;
    m_shared->hasPending = true;
    m_shared->wake.notify_one();
}

void ScanExecutor::shutdown(std::chrono::milliseconds graceMs)
{
    if (m_shutdown) {
        return;
    }
    m_shutdown = true;

    std::shared_ptr<Shared> shared = m_shared;
    {
        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->stopping = true;
        shared->pending  = Job();
        shared->hasPending = false;
    }
    shared->wake.notify_all();

    if (!m_thread.joinable()) {
        return;
    }
    {
        std::unique_lock<std::mutex> lock(shared->mutex);
        if (shared->idle.wait_for(lock, graceMs, [&shared]() { return shared->exited; })) {
            m_thread.join();
            return;
        }
    }
    // 超时：线程还卡在 open() / read() 里。ADR 0021 决策 7 明确允许 detach——它只持有
    // shared 与请求对象，目标对象析构后结果会被弱引用丢弃。
    m_thread.detach();
}

} // namespace sdk
