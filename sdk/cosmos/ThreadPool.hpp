#pragma once
#include "NonCopyable.hpp"
#include "SyncQueue.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <list>
#include <memory>
#include <thread>

class ThreadPool : public NonCopyable {
public:
    enum class ThreadState { kInit = 0, kWaiting, kRunning, kStop };
    enum class ThreadFlag { kInit = 0, kCore, kCache };
    using Task              = std::function<void()>;
    using PoolSeconds       = std::chrono::seconds;
    using ThreadPtr         = std::shared_ptr<std::thread>;
    using ThreadID          = std::atomic<size_t>;
    using ThreadStateAtomic = std::atomic<ThreadState>;
    using ThreadFlagAtomic  = std::atomic<ThreadFlag>;
    using ThreadPoolLock    = std::unique_lock<std::mutex>;
    struct ThreadPoolConfig {
        size_t coreThreads       = std::thread::hardware_concurrency();
        size_t maxThreads        = std::thread::hardware_concurrency();
        size_t maxTaskSize       = 100;
        PoolSeconds cacheTimeout = PoolSeconds(2);

        ThreadPoolConfig() = default;

        ThreadPoolConfig(size_t coreThreadNum, size_t maxThreadNum, PoolSeconds cacheTimeoutSec)
            : coreThreads(coreThreadNum)
            , maxThreads(maxThreadNum)
            , cacheTimeout(cacheTimeoutSec)
        {
        }
    };
    struct ThreadWrapper {
        ThreadPtr ptr;
        ThreadID id;
        ThreadStateAtomic state;
        ThreadFlagAtomic flag;
        ThreadWrapper()
            : ptr(nullptr)
            , id(0)
            , state(ThreadState::kInit)
            , flag(ThreadFlag::kInit)
        {
        }
    };
    using ThreadWrapperPtr = std::shared_ptr<ThreadWrapper>;

public:
    ThreadPool()
        : m_cfg()
        , m_queue(m_cfg.maxTaskSize)
    {
        init();
    }

    ThreadPool(size_t coreThreadNum, size_t maxThreadNum, size_t cacheTimeoutSec)
        : m_cfg(coreThreadNum, maxThreadNum, PoolSeconds(cacheTimeoutSec))
        , m_queue(m_cfg.maxTaskSize)
    {
    }

    ThreadPool(ThreadPoolConfig cfg)
        : m_cfg(cfg)
        , m_queue(m_cfg.maxTaskSize)
    {
        init();
    }

    ~ThreadPool() { stop(); }

    bool start()
    {
        if (!m_available.load()) {
            return false;
        }
        std::call_once(m_flag, [this] { startThread(); });

        return true;
    }

    bool stop(bool isShutdownNow = true)
    {
        if (!m_available.load()) {
            return false;
        }
        std::call_once(m_flag, [this, isShutdownNow] { shutdown(isShutdownNow); });
        return true;
    }

    template <typename F, typename... ARG> bool runTask(F f, ARG ...args)
    {
        if (!m_available.load() || !m_running.load() || m_shutdown.load()
            || m_shutdownNow.load()) {
            return false;
        }

        Task task = std::bind(f, args...);

        m_queue.Put(task);
        m_coreThreadCond.notify_one();
        return true;
    }

    template <typename T, typename F, typename... ARG>
    bool runTaskWithObj(T *obj, F f, ARG ...args)
    {
        if (!m_available.load() || !m_running.load() || m_shutdown.load()
            || m_shutdownNow.load()) {
            return false;
        }

        Task task = std::bind(std::mem_fn(f), obj, args...);

        m_queue.Put(task);
        m_coreThreadCond.notify_one();
        return true;
    }

protected:
    void threadloop(ThreadWrapperPtr threadPtr)
    {
        while (m_running.load()) {
            if (m_shutdownNow.load()) {
                break;
            }
            if (threadPtr->state.load() != ThreadState::kRunning) {
                break;
            }

            size_t taskSize = m_queue.Size();
            if (taskSize == 0) {
                m_waitingThreadNum++;
                if (threadPtr->flag.load() == ThreadFlag::kCore) {
                    ThreadPoolLock lock(m_coreThreadMutex);
                    m_coreThreadCond.wait(lock);
                } else if (threadPtr->flag.load() == ThreadFlag::kCache) {
                    ThreadPoolLock lock(m_cacheThreadMutex);
                    auto state = m_cacheThreadCond.wait_for(lock, m_cfg.cacheTimeout);
                    if (state == std::cv_status::timeout) {
                        threadPtr->state.store(ThreadState::kStop);
                        m_waitingThreadNum--;
                        break;
                    }
                }
            }

            m_waitingThreadNum--;
            taskSize = m_queue.Size();
            if (m_waitingThreadNum.load() == 0 && taskSize > m_cfg.coreThreads) {
                addCacheThread();
            }

            Task task;
            m_queue.Take(task);

            task();
        }
        return;
    }
    bool isValidConfig(const ThreadPoolConfig &cfg)
    {
        return !(cfg.coreThreads < 1 || cfg.coreThreads < cfg.maxThreads
            || cfg.cacheTimeout < PoolSeconds(1));
    }
    void addCoreThread()
    {
        ThreadWrapperPtr ptr = std::make_shared<ThreadWrapper>();
        m_coreThreadGroup.emplace_back(ptr);
        ptr->id.store((size_t)ptr.get());
        ptr->state.store(ThreadState::kRunning);
        ptr->flag.store(ThreadFlag::kCore);
        ThreadPtr thd(new std::thread(&ThreadPool::threadloop, this, ptr));
        ptr->ptr = thd;
    }
    void addCacheThread()
    {
        auto iter = m_cacheThreadGroup.begin();
        for (; iter != m_cacheThreadGroup.end();) {
            if ((*iter)->state.load() == ThreadState::kStop) {
                if ((*iter)->ptr->joinable()) {
                    (*iter)->ptr->join();
                }
                m_cacheThreadGroup.erase(iter);
            } else {
                iter++;
            }
        }

        ThreadWrapperPtr ptr = std::make_shared<ThreadWrapper>();
        m_cacheThreadGroup.emplace_back(ptr);
        ptr->id.store((size_t)ptr.get());
        ptr->state.store(ThreadState::kRunning);
        ptr->flag.store(ThreadFlag::kCore);
        ThreadPtr thd(new std::thread(&ThreadPool::threadloop, this, ptr));
        ptr->ptr = thd;
    }
    void init()
    {
        m_totalTaskNum.store(0);
        m_waitingThreadNum.store(0);
        m_shutdown.store(false);
        m_shutdownNow.store(false);
        m_running.store(false);
        m_coreThreadGroup.clear();
        m_cacheThreadGroup.clear();

        if (!isValidConfig(m_cfg)) {
            m_available.store(false);
        } else {
            m_available.store(true);
        }
    }
    void startThread()
    {
        for (size_t i = 0; i < m_cfg.coreThreads; ++i) {
            addCoreThread();
        }
        for (size_t i = 0; i < m_cfg.maxThreads - m_cfg.coreThreads; ++i) {
            addCacheThread();
        }
        m_running.store(true);
    }
    void shutdown(bool isShutdownNow)
    {
        m_shutdown.store(!isShutdownNow);
        m_shutdownNow.store(isShutdownNow);
        for (auto &temp : m_coreThreadGroup) {
            if (temp->ptr->joinable()) {
                temp->ptr->join();
            }
        }
        for (auto &temp : m_cacheThreadGroup) {
            if (temp->ptr->joinable()) {
                temp->ptr->join();
            }
        }

        m_coreThreadGroup.clear();
        m_cacheThreadGroup.clear();
        m_running.store(false);
    }

    ThreadPoolConfig m_cfg;
    std::list<ThreadWrapperPtr> m_coreThreadGroup;
    std::mutex m_coreThreadMutex;
    std::condition_variable m_coreThreadCond;
    std::list<ThreadWrapperPtr> m_cacheThreadGroup; // 空闲线程组
    std::mutex m_cacheThreadMutex;
    std::condition_variable m_cacheThreadCond;
    SyncQueue<Task> m_queue;
    std::atomic<size_t> m_totalTaskNum;
    std::atomic<size_t> m_waitingThreadNum;
    std::atomic<bool> m_running;
    std::atomic<bool> m_available;
    std::atomic<bool> m_shutdown;
    std::atomic<bool> m_shutdownNow;
    std::once_flag m_flag;
};
