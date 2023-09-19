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
        size_t intervalTimeMs;
        std::atomic<bool> isRepeat;
        std::atomic<bool> available;
        std::atomic<bool> isSync;
        std::future<bool> future;
        std::promise<bool> promise;
    };
    using TaskWrapperPtr = std::shared_ptr<TaskWrapper>;
    struct CompareTasks {
        bool operator()(const TaskWrapperPtr &t1, const TaskWrapperPtr &t2)
        {
            return t1->runTimePointUs < t2->runTimePointUs;
        }
    };
    using TaskQueue
        = std::priority_queue<TaskWrapperPtr, std::vector<TaskWrapperPtr>, CompareTasks>;
    using TaskMap = std::unordered_map<TaskID, TaskWrapperPtr>;

public:
    WorkQueue() { start(); }
    ~WorkQueue() { stop(); }

    template <typename F, typename... Arg>
    TaskID startTimerTask(bool isRepeat, int intervalTimeMs, F f, Arg... args)
    {
        if (!m_running.load()) {
            return TaskID_Error;
        }

        TaskWrapperPtr task  = std::make_shared<TaskWrapper>();
        task->func           = std::bind(f, args...);
        task->intervalTimeMs = intervalTimeMs;
        task->runTimePointUs
            = Timer::getCurrentTimePoint<Timer::microsecond_t>() + task->intervalTimeMs * 1000;
        task->id = (size_t)task.get();
        task->available.store(true);
        task->isRepeat.store(isRepeat);
        task->isSync.store(false);
        {
            std::unique_lock<std::mutex> lock(m_taskMtx);
            m_taskMap[task->id] = task;
            m_taskQueue.push(task);
            m_taskCV.notify_one();
        }
        return task->id;
    }

    template <typename T, typename F, typename... Arg>
    TaskID startTimerTaskWithObj(bool isRepeat, int intervalTimeMs, T *obj, F f, Arg... args)
    {
        return startTimerTask(isRepeat, intervalTimeMs, std::bind(f, obj, args...));
    }

    void stopTimerTask(TaskID id)
    {
        // FIXME: Available uses atomic variables, so does it need to be locked here?
        std::lock_guard<std::mutex> lock(m_taskMtx);
        auto iter = m_taskMap.find(id);
        if (iter == m_taskMap.end()) {
            return;
        }
        m_taskMap[id]->available.store(false);
        m_taskCV.notify_one();
    }

    template <typename F, typename... Arg> void asyncRunTask(F f, Arg... args)
    {
        if (!m_running.load()) {
            return;
        }

        TaskWrapperPtr task  = std::make_shared<TaskWrapper>();
        task->func           = std::bind(f, args...);
        task->intervalTimeMs = 0;
        task->runTimePointUs = Timer::getCurrentTimePoint<Timer::microsecond_t>();
        task->id             = (size_t)task.get();
        task->available.store(true);
        task->isRepeat.store(false);
        task->isSync.store(false);
        {
            std::unique_lock<std::mutex> lock(m_taskMtx);
            m_taskMap[task->id] = task;
            m_taskQueue.push(task);
            m_taskCV.notify_one();
        }
        return;
    }

    template <typename T, typename F, typename... Arg>
    void asyncRunTaskWithObj(T *obj, F f, Arg... args)
    {
        asyncRunTask(std::bind(f, obj, args...));
    }

    template <typename F, typename... Arg> void syncRunTask(F f, Arg... args)
    {
        if (!m_running.load()) {
            return;
        }

        TaskWrapperPtr task  = std::make_shared<TaskWrapper>();
        task->func           = std::bind(f, args...);
        task->intervalTimeMs = 0;
        task->runTimePointUs = Timer::getCurrentTimePoint<Timer::microsecond_t>();
        task->id             = (size_t)task.get();
        task->available.store(true);
        task->isRepeat.store(false);
        task->isSync.store(true);
        task->future = task->promise.get_future();
        {
            std::unique_lock<std::mutex> lock(m_taskMtx);
            m_taskMap[task->id] = task;
            m_taskQueue.push(task);
            m_taskCV.notify_one();
        }
        task->future.wait();
        return;
    }

    template <typename T, typename F, typename... Arg>
    void syncRunTaskWithObj(T *obj, F f, Arg... args)
    {
        syncRunTask(std::bind(f, obj, args...));
    }

protected:
    void start()
    {
        m_thread = std::thread(&WorkQueue::threadLoopFunc, this);
        m_running.store(true);
    }
    void stop()
    {
        m_running.store(false);
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }
    void threadLoopFunc()
    {
        while (m_running.load()) {
            size_t taskSize;
            TaskWrapperPtr task;
            {
                std::unique_lock<std::mutex> lock(m_taskMtx);
                taskSize = m_taskMap.size();
                if (taskSize == 0) {
                    m_taskCV.wait(lock);
                    continue;
                }

                task = m_taskQueue.top();
                if (task->available.load()) {
                    auto nowTimeUs = Timer::getCurrentTimePoint<Timer::microsecond_t>();
                    if (nowTimeUs < task->runTimePointUs) {
                        auto state = m_taskCV.wait_for(
                            lock, Timer::microsecond_t((task->runTimePointUs - nowTimeUs)));
                        if (state == std::cv_status::no_timeout) {
                            continue;
                        }
                    }
                }
                m_taskQueue.pop();
                m_taskMap.erase(task->id);
            }

            if (task->available.load()) {
                task->func();
                if (task->isSync.load()) {
                    task->promise.set_value(true);
                }
                if (task->isRepeat) {
                    task->runTimePointUs = Timer::getCurrentTimePoint<Timer::microsecond_t>()
                        + task->intervalTimeMs * 1000;
                    std::lock_guard<std::mutex> lock(m_taskMtx);
                    m_taskQueue.push(task);
                    m_taskMap[task->id] = task;
                }
            }
        }
        return;
    }

private:
    TaskQueue m_taskQueue;
    TaskMap m_taskMap;
    std::mutex m_taskMtx;
    std::condition_variable m_taskCV;
    std::thread m_thread;
    std::atomic<bool> m_running;
};
