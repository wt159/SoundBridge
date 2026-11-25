#pragma once

#include "NonCopyable.hpp"
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace sdk {

// 目录扫描的专用线程（ADR 0021 决策 1 / 7，地图 #313 → 执行票 #320）。
//
// 背景：`recursiveFileSearch` 在生产路径上跑在 UI 线程（`MainWindow` 构造期），
// 逐文件探测又在控制用 `WorkQueue` 上按时间戳排队——前者冻结首帧，后者把
// `stop` / `pause` / `seek` 与 10 ms 消费节拍挤在探测后面。两个问题的同一个解：
// 把遍历与探测整体搬到这条线程，控制队列只剩「单条插入」与原有控制命令。
//
// 与 `SwitchOpenExecutor` 的差别（ADR 0021 决策 4）：本执行器**不**做有界等待，
// 作业自己负责把结果回投控制队列；队列深度为 1，新作业替换尚未开始的旧作业
//（后者胜），正在执行的旧作业由调用方通过请求的 cancelled 位作废。
//
// 退场（ADR 0021 决策 7，把 ADR 0020 决策 5 的让步扩展到两类 IO 线程）：shutdown
// 最多等 graceMs，仍未返回就 detach。滞留线程只持有本文件的共享状态与请求对象，
// 不引用 MusicPlayList / MusicPlayer；结果经弱引用投递，目标已析构则丢弃。
// **不允许**为了「无线程泄漏」改成无限 join——退出会被 SMB 60 s / NFS 无限重试顶住。
class ScanExecutor : NonCopyable {
public:
    using Job = std::function<void()>;

    ScanExecutor();
    ~ScanExecutor();

    // 提交一次扫描作业，立即返回。尚未开始的旧作业被直接丢弃（调用方负责先把
    // 旧请求置为 cancelled，保证正在执行的那一份也会在一个文件内退出）。
    void submit(const Job &job);

    // 停止接受新作业，至多等 graceMs；超时 detach（见类注释）。
    void shutdown(std::chrono::milliseconds graceMs);

private:
    // 必须由 shared_ptr 持有：detach 之后的线程仍要访问它。
    struct Shared {
        std::mutex mutex;
        std::condition_variable wake;
        std::condition_variable idle;
        Job pending;
        bool hasPending = false;
        bool stopping   = false;
        bool exited     = false;
    };

    std::shared_ptr<Shared> m_shared;
    std::thread m_thread;
    bool m_shutdown = false;
};

} // namespace sdk
