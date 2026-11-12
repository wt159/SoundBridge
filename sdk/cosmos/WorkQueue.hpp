#pragma once
#include "NonCopyable.hpp"
#include "Timer.hpp"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

class WorkQueue : NonCopyable {
public:
    enum TaskID_Value {
        TaskID_Error = 0,
    };
    using TaskID = size_t;

private:
    struct TaskWrapper {
        TaskID id;
        std::function<void()> func;
        size_t runTimePointUs;
        size_t intervalTimeUs;
        std::atomic<bool> isRepeat;
        std::atomic<bool> available;
        std::atomic<bool> isSync;
        // 承诺只允许兑现一次（true=已执行 / false=未执行被丢弃）。
        // 工作线程的兑现点与 stop() 的清算点靠 join 互斥，此标志是第二道防线，
        // 防止未来改动引入双 set_value（会抛 future_error 并 terminate 掉线程）。
        std::atomic<bool> promiseFulfilled{false};
        std::future<bool> future;
        std::promise<bool> promise;
    };
    using TaskWrapperPtr = std::shared_ptr<TaskWrapper>;
    struct CompareTasks {
        bool operator()(const TaskWrapperPtr &t1, const TaskWrapperPtr &t2)
        {
            return t1->runTimePointUs > t2->runTimePointUs;
        }
    };
    using TaskQueue
        = std::priority_queue<TaskWrapperPtr, std::vector<TaskWrapperPtr>, CompareTasks>;
    using TaskMap = std::unordered_map<TaskID, TaskWrapperPtr>;

public:
    WorkQueue() { start(); }
    ~WorkQueue() { stop(); }

    // 显式停止并等待队列线程退出（幂等）。析构会自动调用；需要「保证此刻没有任务在跑」
    // 的场景可以提前调用——例如 MusicPlayer::Impl 析构必须在关音频设备前静止控制队列，
    // 否则队列线程里的 _stop 会与析构并发进入 SDL_CloseAudioDevice（issue #70）。
    // 调用后不再接受新任务；已入队但未执行的同步任务会被清算为 false（不挂死调用方），
    // 异步任务直接丢弃（与旧行为一致：工作线程退出路径上同样不执行）。
    void shutdown() { stop(); }

    template <typename F, typename... Arg>
    TaskID startTimerTask(bool isRepeat, int intervalTimeMs, F &&f, Arg &&...args)
    {
        if (!m_running.load()) {
            return TaskID_Error;
        }

        TaskWrapperPtr task  = std::make_shared<TaskWrapper>();
        task->func           = std::bind(std::forward<F>(f), std::forward<Arg>(args)...);
        task->intervalTimeUs = intervalTimeMs * 1000;
        task->runTimePointUs
            = Timer::getCurrentTimePoint<Timer::microsecond_t>() + task->intervalTimeUs;
        // id 必须用单调计数器而非对象地址（issue #142）：任务销毁后地址会被复用，
        // 两个任务撞同一个 id 时 m_taskMap[id] = task 会静默覆盖别人的条目，
        // map 与 queue 从此分叉。计数器从 1 起，0 保留给 TaskID_Error。
        task->id = m_nextTaskID.fetch_add(1);
        task->available.store(true);
        task->isRepeat.store(isRepeat);
        task->isSync.store(false);
        {
            std::unique_lock<std::mutex> lock(m_taskMtx);
            // 判定必须在锁内：否则「检查通过 → stop() 完成 → 再入队」会把任务
            // 塞进一个已经没有工作线程的队列（issue #142 挂死形态之一）。
            if (!m_running.load()) {
                return TaskID_Error;
            }
            m_taskMap[task->id] = task;
            m_taskQueue.push(task);
            m_taskCV.notify_one();
        }
        return task->id;
    }

    void stopTimerTask(TaskID id)
    {
        std::lock_guard<std::mutex> lock(m_taskMtx);
        auto iter = m_taskMap.find(id);
        if (iter == m_taskMap.end()) {
            return;
        }
        iter->second->available.store(false);
        m_taskCV.notify_one();
    }

    template <typename F, typename... Arg> void asyncRunTask(F &&f, Arg &&...args)
    {
        if (!m_running.load()) {
            return;
        }

        TaskWrapperPtr task  = std::make_shared<TaskWrapper>();
        task->func           = std::bind(std::forward<F>(f), std::forward<Arg>(args)...);
        task->intervalTimeUs = 0;
        task->runTimePointUs = Timer::getCurrentTimePoint<Timer::microsecond_t>();
        task->id             = m_nextTaskID.fetch_add(1);
        task->available.store(true);
        task->isRepeat.store(false);
        task->isSync.store(false);
        {
            std::unique_lock<std::mutex> lock(m_taskMtx);
            if (!m_running.load()) {
                return;
            }
            m_taskMap[task->id] = task;
            m_taskQueue.push(task);
            m_taskCV.notify_one();
        }
    }

    template <typename F, typename... Arg> void syncRunTask(F &&f, Arg &&...args)
    {
        if (!m_running.load()) {
            return;
        }

        TaskWrapperPtr task  = std::make_shared<TaskWrapper>();
        task->func           = std::bind(std::forward<F>(f), std::forward<Arg>(args)...);
        task->intervalTimeUs = 0;
        task->runTimePointUs = Timer::getCurrentTimePoint<Timer::microsecond_t>();
        task->id             = m_nextTaskID.fetch_add(1);
        task->available.store(true);
        task->isRepeat.store(false);
        task->isSync.store(true);
        task->future = task->promise.get_future();
        {
            std::unique_lock<std::mutex> lock(m_taskMtx);
            if (!m_running.load()) {
                // 不入队：把承诺清算为「未执行」，本函数直接返回，
                // 不走到下面的 future.wait()。
                if (!task->promiseFulfilled.exchange(true)) {
                    task->promise.set_value(false);
                }
                return;
            }
            m_taskMap[task->id] = task;
            m_taskQueue.push(task);
            m_taskCV.notify_one();
        }
        task->future.wait();
    }

protected:
    void start()
    {
        // 先置位再起线程：旧顺序（先起线程后置 true）下工作线程可能在
        // threadLoopFunc 首行读到未初始化的 m_running（UB，issue #142 可疑点 3）。
        m_running.store(true);
        m_thread = std::thread(&WorkQueue::threadLoopFunc, this);
    }
    void stop()
    {
        {
            // 在锁内置 false：与三个插入路径的锁内检查配对成对出现，
            // 保证 stop() 返回后不可能再有任务入队。
            std::lock_guard<std::mutex> lock(m_taskMtx);
            m_running.store(false);
            m_taskCV.notify_all();
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
        // join 之后队列里残留的任务不会再被执行；同步任务的承诺必须在此清算，
        // 否则 syncRunTask 的 future.wait() 永久挂起（issue #142 挂死形态的近亲）。
        std::lock_guard<std::mutex> lock(m_taskMtx);
        while (!m_taskQueue.empty()) {
            TaskWrapperPtr task = m_taskQueue.top();
            if (task->isSync.load() && !task->promiseFulfilled.exchange(true)) {
                task->promise.set_value(false);
            }
            m_taskQueue.pop();
        }
        m_taskMap.clear();
    }
    void threadLoopFunc()
    {
        while (m_running.load()) {
            TaskWrapperPtr task;
            {
                std::unique_lock<std::mutex> lock(m_taskMtx);
                // 「是否无事可做」以队列自身为唯一真源（issue #142 崩溃形态：
                // 旧代码用 m_taskMap.size() 当判据，map 与 queue 一旦分叉就会
                // 在空队列上 top()/pop()——core 实测 m_taskMap.size()==1 而
                // m_taskQueue.c.size()==-1，即 pop_back 把 _M_finish 减穿 _M_start）。
                if (m_taskQueue.empty()) {
                    m_taskCV.wait(lock);
                    continue;
                }

                task = m_taskQueue.top();
                if (!task->available.load()) {
                    m_taskQueue.pop();
                    m_taskMap.erase(task->id);
                    continue;
                }

                size_t nowTimeUs = Timer::getCurrentTimePoint<Timer::microsecond_t>();
                ssize_t difftime = task->runTimePointUs - nowTimeUs;
                if (difftime > 0) {
                    // wait_for 期间锁被释放：其它线程可能插入 runTimePointUs 更早
                    // 的任务成为新队顶；并且「notify 恰好落在超时已触发、尚未重新
                    // 加锁的窗口内」时唤醒会丢失。两种情况醒来后手里的 task 都
                    // 可能已不是队顶——必须复核，否则 pop() 摘掉的是别人的任务、
                    // erase() 删的却是自己的条目，map 与 queue 成员从此分叉：
                    // 队列幽灵被再次摘除时 erase 无操作 → map 多一条 → 下一轮在
                    // 空队列上 top()（崩溃）；map 幽灵永不执行 → 任务无声丢失（挂死）。
                    // 这就是 #142 的根因链。
                    std::cv_status state
                        = m_taskCV.wait_for(lock, Timer::microsecond_t(difftime));
                    if (state == std::cv_status::no_timeout) {
                        continue;
                    }
                    if (m_taskQueue.empty() || task != m_taskQueue.top()) {
                        continue;
                    }
                }

                if (!task->isRepeat.load()) {
                    m_taskQueue.pop();
                    m_taskMap.erase(task->id);
                } else {
                    // 先 pop 再改 key：在堆内修改参与堆序比较的 key 违反
                    // priority_queue 的前提（issue #142 可疑点 4）。
                    m_taskQueue.pop();
                    task->runTimePointUs += task->intervalTimeUs;
                    m_taskQueue.push(task);
                }
            }

            if (!m_running.load()) {
                // 任务已出队但队列已停：不再执行，只清算同步承诺。
                if (task->isSync.load() && !task->promiseFulfilled.exchange(true)) {
                    task->promise.set_value(false);
                }
                break;
            }

            task->func();
            if (task->isSync.load() && !task->promiseFulfilled.exchange(true)) {
                task->promise.set_value(true);
            }
        }
    }

private:
    TaskQueue m_taskQueue;
    TaskMap m_taskMap;
    std::mutex m_taskMtx;
    std::condition_variable m_taskCV;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<size_t> m_nextTaskID{1};
};
