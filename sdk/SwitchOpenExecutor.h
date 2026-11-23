#pragma once

#include "NonCopyable.hpp"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace sdk {

// 切歌重开的专用 IO 执行器（ADR 0020，地图 #285 / #287）。
//
// 背景：本地 open 没有超时参数、也换不来可靠取消（#286）。唯一能保证「控制命令
// 不排队等系统调用」的办法，是把 open 放到另一条线程上，控制线程只做**有界等待**：
// 等 50 ms 拿结果，超时就把收尾交给完成回调。执行器本身串行：同一时刻至多一次
// 重开，句柄预算与「在途 open 不可取消」的代价都可推。
//
// 退场（ADR 0020 决策 5）：shutdown 最多等 graceMs，仍未返回就 detach。滞留线程
// 只持有本文件的共享状态与请求值，不引用 MusicPlayList / MusicPlayer，进程退出
// 由 OS 回收。**不允许**为了「无线程泄漏」改成无限 join——退出会被 SMB 60 s /
// NFS 无限重试顶住。
class SwitchOpenExecutor : NonCopyable {
public:
    enum class WaitResult {
        Completed, // job 已返回，调用线程可安全读取 job 写入的结果
        TimedOut,  // job 仍在执行或排队；调用方放弃等待，由 job 自己完成后续动作
        Stopped,   // 执行器已停止，job 不会执行
    };

    SwitchOpenExecutor();
    ~SwitchOpenExecutor();

    // 把 job 提交到 IO 线程，至多阻塞 waitMs。job 内部负责把「完成之后要做什么」
    // 投回控制队列；本方法不替调用方做任何回调。
    WaitResult runBounded(std::chrono::milliseconds waitMs, const std::function<void()> &job);

    // 停止接受新任务，至多等 graceMs。超时则 detach（见类注释）。
    void shutdown(std::chrono::milliseconds graceMs);

private:
    struct Job {
        std::mutex mutex;
        std::condition_variable done;
        std::function<void()> fn;
        bool finished = false;
    };

    // 必须由 shared_ptr 持有：detach 之后的线程仍要访问它。
    struct Shared {
        std::mutex mutex;
        std::condition_variable wake;
        std::condition_variable idle;
        std::deque<std::shared_ptr<Job>> jobs;
        bool stopping = false;
        bool exited   = false;
    };

    std::shared_ptr<Shared> m_shared;
    std::thread m_thread;
    bool m_shutdown = false;
};

} // namespace sdk
