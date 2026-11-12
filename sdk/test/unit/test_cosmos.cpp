#include <Lazy.hpp>
#include <Optional.hpp>
#include <Range.hpp>
#include <ScopeGuard.hpp>
#include <Timer.hpp>
#include <WorkQueue.hpp>
#include <doctest/doctest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_name.hpp>
#include <vector>

TEST_SUITE("Cosmos::Optional")
{
    TEST_CASE("Empty state")
    {
        Optional<int> opt;
        REQUIRE(opt.isInit() == false);
    }

    TEST_CASE("Value construction")
    {
        Optional<int> opt(42);
        REQUIRE(opt.isInit() == true);
        REQUIRE(*opt == 42);
    }

    TEST_CASE("Emplace")
    {
        Optional<std::string> opt;
        opt.emplace(5, 'x');
        REQUIRE(opt.isInit() == true);
        REQUIRE(*opt == "xxxxx");
    }

    TEST_CASE("Copy constructor")
    {
        Optional<int> opt1(100);
        Optional<int> opt2(opt1);
        REQUIRE(opt2.isInit() == true);
        REQUIRE(*opt2 == 100);
    }

    TEST_CASE("Move constructor")
    {
        Optional<std::string> opt1(std::string("hello"));
        Optional<std::string> opt2(std::move(opt1));
        REQUIRE(opt2.isInit() == true);
    }

    TEST_CASE("Assignment operator")
    {
        Optional<int> opt1;
        Optional<int> opt2(50);
        opt1 = opt2;
        REQUIRE(opt1.isInit() == true);
        REQUIRE(*opt1 == 50);
    }

    TEST_CASE("Reset to empty")
    {
        Optional<int> opt(123);
        REQUIRE(opt.isInit() == true);
        opt = Optional<int>();
        REQUIRE(opt.isInit() == false);
    }

    TEST_CASE("Throw on dereference empty")
    {
        Optional<int> opt;
        REQUIRE_THROWS(*opt);
    }

    TEST_CASE("Comparison - equal")
    {
        Optional<int> opt1(10);
        Optional<int> opt2(10);
        REQUIRE(opt1 == opt2);
    }

    TEST_CASE("Comparison - not equal")
    {
        Optional<int> opt1(10);
        Optional<int> opt2(20);
        REQUIRE(opt1 != opt2);
    }

    TEST_CASE("Comparison - less than")
    {
        Optional<int> opt1(10);
        Optional<int> opt2(20);
        REQUIRE(opt1 < opt2);
    }

    TEST_CASE("Comparison - empty equals empty")
    {
        Optional<int> opt1;
        Optional<int> opt2;
        REQUIRE(opt1 == opt2);
    }

    TEST_CASE("Boolean conversion")
    {
        Optional<int> empty;
        Optional<int> filled(1);
        REQUIRE_FALSE(static_cast<bool>(empty));
        REQUIRE(static_cast<bool>(filled));
    }
}

TEST_SUITE("Cosmos::Lazy")
{
    TEST_CASE("Deferred initialization")
    {
        int constructionCount = 0;
        auto factory          = [&constructionCount]() -> std::shared_ptr<int> {
            constructionCount++;
            return std::make_shared<int>(42);
        };

        Lazy<std::shared_ptr<int>> lazy(factory);

        REQUIRE(constructionCount == 0);
        REQUIRE(lazy.IsValueCreated() == false);

        auto &value = lazy.Value();

        REQUIRE(constructionCount == 1);
        REQUIRE(lazy.IsValueCreated() == true);
        REQUIRE(*value == 42);

        auto &value2 = lazy.Value();
        REQUIRE(constructionCount == 1);
        (void)value2;
    }

    TEST_CASE("Lazy returns reference")
    {
        auto factory = []() -> int { return 100; };

        Lazy<int> lazy(factory);
        REQUIRE(lazy.IsValueCreated() == false);

        int &value = lazy.Value();
        REQUIRE(lazy.IsValueCreated() == true);
        REQUIRE(value == 100);

        value       = 200;
        int &value2 = lazy.Value();
        REQUIRE(value2 == 200);
    }
}

TEST_SUITE("Cosmos::ScopeGuard")
{
    TEST_CASE("MakeGuard creates scope guard")
    {
        auto guard = MakeGuard([]() {});
        (void)guard;
    }

    TEST_CASE("Dismiss works")
    {
        auto guard = MakeGuard([]() {});
        guard.Dismiss();
    }
}

TEST_SUITE("Cosmos::Range")
{
    TEST_CASE("Range with positive step")
    {
        int sum = 0;
        for (int i : Range(0, 10, 2)) {
            sum += i;
        }
        REQUIRE(sum == 20);
    }

    TEST_CASE("Range with default step")
    {
        int sum = 0;
        for (int i : Range(0, 5)) {
            sum += i;
        }
        REQUIRE(sum == 10);
    }

    TEST_CASE("Range with single argument")
    {
        int count = 0;
        for (int i : Range(5)) {
            (void)i;
            count++;
        }
        REQUIRE(count == 5);
    }

    TEST_CASE("Range with negative step")
    {
        int sum = 0;
        for (int i : Range(10, 0, -2)) {
            sum += i;
        }
        REQUIRE(sum == 30);
    }

    TEST_CASE("Range size")
    {
        REQUIRE(Range(0, 10, 2).size() == 5);
        REQUIRE(Range(0, 5).size() == 5);
        REQUIRE(Range(5).size() == 5);
    }

    TEST_CASE("Range operator access")
    {
        REQUIRE(Range(0, 10, 2)[0] == 0);
        REQUIRE(Range(0, 10, 2)[1] == 2);
        REQUIRE(Range(0, 10, 2)[4] == 8);
    }
}

TEST_SUITE("Cosmos::Timer")
{
    TEST_CASE("Timer default construction")
    {
        Timer timer;
        int64_t elapsed = timer.elapsed();
        CHECK(elapsed >= 0);
    }

    TEST_CASE("Timer reset")
    {
        Timer timer;
        timer.elapsed<Timer::millisecond_t>();
        timer.reset();
        int64_t elapsed = timer.elapsed();
        CHECK(elapsed == 0);
    }

    TEST_CASE("Timer getStartTimestamp")
    {
        Timer timer;
        std::time_t ts = timer.getStartTimestamp();
        CHECK(ts > 0);
    }

    TEST_CASE("Timer getCurrentTimestamp")
    {
        std::time_t ts = Timer::getCurrentTimestamp();
        CHECK(ts > 0);
    }

    TEST_CASE("Timer getCurrentLocalTime")
    {
        Timer::Date date = Timer::getCurrentLocalTime();
        CHECK(date.year > 0);
        CHECK(date.month >= 1);
        CHECK(date.month <= 12);
    }

    TEST_CASE("Timer getCurrentTimePoint")
    {
        size_t tp = Timer::getCurrentTimePoint<Timer::millisecond_t>();
        CHECK(tp > 0);
    }

    TEST_CASE("Timer Date to_str")
    {
        Timer::Date date;
        date.year        = 2024;
        date.month       = 1;
        date.day         = 15;
        date.weekday     = 1;
        date.hour        = 10;
        date.minute      = 30;
        date.second      = 45;
        date.millisecond = 123;

        std::string str = date.to_str();
        CHECK(str.empty() == false);
    }
}

TEST_SUITE("Cosmos::type_name")
{
    TEST_CASE("type_name for int")
    {
        std::string name = type_name<int>();
        CHECK(name.empty() == false);
    }

    TEST_CASE("type_name for const int")
    {
        std::string name = type_name<const int>();
        CHECK(name.empty() == false);
    }

    TEST_CASE("type_name for int reference")
    {
        std::string name = type_name<int &>();
        CHECK(name.empty() == false);
    }

    TEST_CASE("type_name for const reference")
    {
        std::string name = type_name<const int &>();
        CHECK(name.empty() == false);
    }

    TEST_CASE("type_name for volatile")
    {
        std::string name = type_name<volatile int>();
        CHECK(name.empty() == false);
    }
}

// ---------------------------------------------------------------------------
// WorkQueue（issue #142：任务队列堆损坏 / map 与 queue 分叉）
//
// 判据设计说明：
// 1. 「shutdown fulfills pending sync task」是确定性判据——修复缺失（无清算）
//    时调用线程必然永久阻塞在 future.wait()，宽限期一到必红。
// 2. 守卫取值遵守 #140 教训：内部守卫显著宽于被测链路自身预算（这里的链路
//    预算是 300ms 占位任务 + 清算，守卫 2s）。
// 3. 计数类判据（唯一性、全部执行完毕）是精确等值判断，宽限期只作守卫。
// ---------------------------------------------------------------------------
TEST_SUITE("Cosmos::WorkQueue")
{
    TEST_CASE("syncRunTask executes before returning")
    {
        WorkQueue queue;
        std::atomic<bool> ran{false};
        queue.syncRunTask([&ran]() { ran.store(true); });
        CHECK(ran.load());
    }

    TEST_CASE("no new tasks after shutdown")
    {
        WorkQueue queue;
        queue.shutdown();
        CHECK(queue.startTimerTask(false, 1, []() {}) == WorkQueue::TaskID_Error);

        std::atomic<bool> ran{false};
        queue.asyncRunTask([&ran]() { ran.store(true); });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK_FALSE(ran.load());
    }

    TEST_CASE("task ids are unique and never zero")
    {
        WorkQueue queue;
        std::vector<WorkQueue::TaskID> ids;
        // 一次性任务寿命 1ms，创建间隔 3ms：前一个任务必已执行并销毁，
        // 分配器会把它的内存块复用给下一个任务——旧实现（id=对象地址）
        // 在此交错下必然撞 id；计数器实现下 id 与地址复用无关。
        for (int i = 0; i < 30; ++i) {
            WorkQueue::TaskID id = queue.startTimerTask(false, 1, []() {});
            REQUIRE(id != WorkQueue::TaskID_Error);
            ids.push_back(id);
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
        std::sort(ids.begin(), ids.end());
        CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
    }

    TEST_CASE("repeat timer fires until stopped")
    {
        WorkQueue queue;
        std::atomic<int> fired{0};
        WorkQueue::TaskID id = queue.startTimerTask(true, 5, [&fired]() { fired.fetch_add(1); });
        REQUIRE(id != WorkQueue::TaskID_Error);

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        queue.stopTimerTask(id);
        const int afterStop = fired.load();
        // available 只在循环顶检查：已到点的最后一次执行可能仍会发生
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(fired.load() <= afterStop + 1);
        // 5ms 间隔 × 100ms 窗口 ≈ 20 次，下限只做 15 倍余量的粗校验
        CHECK(afterStop >= 3);
    }

    TEST_CASE("shutdown fulfills pending sync task")
    {
        WorkQueue queue;
        std::atomic<bool> syncReturned{false};

        // 占住工作线程 ~300ms，让下面的同步任务有充分时间「已入队、未执行」
        queue.asyncRunTask([]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        });

        std::thread caller([&queue, &syncReturned]() {
            queue.syncRunTask([]() {});
            syncReturned.store(true);
        });

        // 确保同步任务已入队（其调用线程随即阻塞在 future.wait() 上）
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // 此刻置停：工作线程做完手头的占位任务后会因 m_running=false 直接退出，
        // 不会执行排队的同步任务。修复后 stop() 在 join 之后清算队列，把该任务
        // 的 promise 兑现为 false；修复前调用线程永久挂死（#142 挂死形态近亲）。
        queue.shutdown();

        bool returned = false;
        for (int i = 0; i < 400; ++i) { // 2s 守卫 >> 300ms 占位 + 清算预算
            if (syncReturned.load()) {
                returned = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!returned) {
            caller.detach(); // 判红前分离，避免用例收尾时 join 挂死整个进程
        }
        REQUIRE(returned);
        caller.join();
    }

    TEST_CASE("concurrent async submissions all execute")
    {
        WorkQueue queue;
        std::atomic<int> executed{0};
        std::atomic<int> submitted{0};

        std::vector<std::thread> producers;
        for (int p = 0; p < 4; ++p) {
            producers.emplace_back([&queue, &executed, &submitted]() {
                for (int i = 0; i < 50; ++i) {
                    queue.asyncRunTask([&executed]() { executed.fetch_add(1); });
                    submitted.fetch_add(1);
                }
            });
        }
        for (std::thread &producer : producers) {
            producer.join();
        }

        // 判据本体是精确等值（幽灵任务 = 有提交无执行，永远等不齐）；
        // 轮询只是守卫，5s 远宽于 200 个瞬时任务的执行预算。
        bool drained = false;
        for (int i = 0; i < 1000; ++i) {
            if (executed.load() == submitted.load()) {
                drained = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(drained);
        queue.shutdown();
    }

    TEST_CASE("timer start/stop churn ends cleanly")
    {
        // 反复起停定时器（MusicPlayer 消费线程的真实使用形态），
        // 判据 = 全程不崩、shutdown 不挂死（由 ctest TIMEOUT 兜底）。
        WorkQueue queue;
        std::atomic<int> fired{0};
        std::vector<WorkQueue::TaskID> ids;
        for (int i = 0; i < 30; ++i) {
            ids.push_back(
                queue.startTimerTask(true, 3 + (i % 7), [&fired]() { fired.fetch_add(1); }));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        for (size_t k = 0; k < ids.size(); k += 2) {
            queue.stopTimerTask(ids[k]);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        queue.shutdown();
        CHECK(fired.load() >= 0); // 烟雾校验：主要判据是本用例完整跑完
    }
}
