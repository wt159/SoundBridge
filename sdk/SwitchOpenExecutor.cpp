#include "SwitchOpenExecutor.h"

namespace sdk {

SwitchOpenExecutor::SwitchOpenExecutor()
    : m_shared(std::make_shared<Shared>())
{
    std::shared_ptr<Shared> shared = m_shared;
    m_thread = std::thread([shared]() {
        for (;;) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock<std::mutex> lock(shared->mutex);
                shared->wake.wait(lock, [&shared]() {
                    return shared->stopping || !shared->jobs.empty();
                });
                if (shared->stopping && shared->jobs.empty()) {
                    shared->exited = true;
                    shared->idle.notify_all();
                    return;
                }
                job = shared->jobs.front();
                shared->jobs.pop_front();
            }
            if (job->fn) {
                job->fn();
            }
            {
                // 与 runBounded 的等待配对：done=true 之后，job 写入的结果对
                // 唤醒方可见（互斥量提供 happens-before）。
                std::lock_guard<std::mutex> lock(job->mutex);
                job->finished = true;
            }
            job->done.notify_all();
        }
    });
}

SwitchOpenExecutor::~SwitchOpenExecutor()
{
    shutdown(std::chrono::milliseconds(200));
}

SwitchOpenExecutor::WaitResult SwitchOpenExecutor::runBounded(std::chrono::milliseconds waitMs,
                                                              const std::function<void()> &job)
{
    std::shared_ptr<Job> request = std::make_shared<Job>();
    request->fn                  = job;
    {
        std::lock_guard<std::mutex> lock(m_shared->mutex);
        if (m_shared->stopping) {
            return WaitResult::Stopped;
        }
        m_shared->jobs.push_back(request);
    }
    m_shared->wake.notify_one();

    std::unique_lock<std::mutex> lock(request->mutex);
    if (request->done.wait_for(lock, waitMs, [&request]() { return request->finished; })) {
        return WaitResult::Completed;
    }
    return WaitResult::TimedOut;
}

void SwitchOpenExecutor::shutdown(std::chrono::milliseconds graceMs)
{
    if (m_shutdown) {
        return;
    }
    m_shutdown = true;

    std::shared_ptr<Shared> shared = m_shared;
    std::shared_ptr<Job> abandoned;
    {
        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->stopping = true;
        // 尚未执行的请求：本执行器不会再跑它们，把等待方（若有）立刻放行。
        while (!shared->jobs.empty()) {
            abandoned = shared->jobs.front();
            shared->jobs.pop_front();
            std::lock_guard<std::mutex> jobLock(abandoned->mutex);
            abandoned->finished = true;
            abandoned->done.notify_all();
        }
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
    // 超时：线程还卡在 open() 里。ADR 0020 决策 5 明确允许 detach——它只持有
    // shared 与请求值，目标对象析构后结果会被弱引用丢弃。
    m_thread.detach();
}

} // namespace sdk
