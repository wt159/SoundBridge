// pending 切换判据（ADR 0020 / 地图 #285）。
// 本机只有 NVMe SSD：慢介质用 FileSource::setOpenDelayForTest 注入（ADR 0011），
// 让 open 落到 50 ms 等待窗口之外，把「有界等待 + 后台续做」的每条分支变成可判定的：
// pending 提交、pending 失败、stop / 删目标作废、新意图顶替、补位重排、候选续做、
// play 意图。注入开关默认 0，测完由 RAII guard 归零。
#include "../fixtures/SyntheticWav.hpp"
#include "../fixtures/TempMediaDir.hpp"
#include "../mocks/PlayerMocks.hpp"
#include <FileSource.h>
#include <MusicPlayList.h>
#include <MusicPlayer.h>
#include <chrono>
#include <doctest/doctest.h>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace sdk;

namespace {

// 进程级注入开关的 RAII guard：析构归零，不泄漏给同一二进制里的后续用例。
class ScopedOpenDelay {
public:
    explicit ScopedOpenDelay(uint32_t ms) { FileSource::setOpenDelayForTest(ms); }
    ~ScopedOpenDelay() { FileSource::setOpenDelayForTest(0); }
    ScopedOpenDelay(const ScopedOpenDelay &)            = delete;
    ScopedOpenDelay &operator=(const ScopedOpenDelay &) = delete;
};

// TC / 重开失败 / settlement 三类事件按发生顺序抄录。TC 与 settlement 都可能从
// 控制队列线程到达（pending 回投），加锁后读。
struct SwitchRecordingCallback : public MusicPlayListCallback {
    mutable std::mutex mutex;
    std::vector<int> currentIndexes;             // 每次 TC 的载荷 index
    std::vector<ErrorCode> openFailures;         // onPlaybackOpenFailed 的错误码
    std::vector<int> openFailureIndexes;
    std::vector<SwitchSettlement> settlements;
    std::vector<SwitchOrigin> settlementOrigins;

    void putMusicPlayListCurBuf(MusicPropertiesPtr property) override
    {
        if (property == nullptr) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex);
        currentIndexes.push_back(static_cast<int>(property->index));
    }
    void updateMusicList(std::vector<MusicPropertiesPtr> &) override { }
    void onMusicPlayListError(ErrorCode, const std::string &, int, const std::string &) override
    {
    }
    void onPlaybackOpenFailed(ErrorCode code, const std::string &, int index,
                              const std::string &) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        openFailures.push_back(code);
        openFailureIndexes.push_back(index);
    }
    void onPendingSwitchSettled(SwitchSettlement settlement, SwitchOrigin origin) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        settlements.push_back(settlement);
        settlementOrigins.push_back(origin);
    }

    std::vector<int> snapshotIndexes() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return currentIndexes;
    }
    size_t openFailureCount() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return openFailures.size();
    }
    size_t settlementCount() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return settlements.size();
    }
};

template <typename Predicate>
bool waitFor(Predicate predicate, int timeoutMs, int stepMs = 20)
{
    for (int waited = 0; waited < timeoutMs; waited += stepMs) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(stepMs));
    }
    return predicate();
}

bool writeBytes(const std::string &path, const std::vector<char> &bytes)
{
    std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

// 播放器把控制命令丢进工作队列异步执行，固定 sleep 容易跑在命令前面。
bool waitForState(MusicPlayer &player, MusicPlayerState want, int timeoutMs = 3000)
{
    return waitFor([&player, want]() { return player.state() == want; }, timeoutMs);
}

AudioSpec pendingTestSpec()
{
    AudioSpec devSpec;
    devSpec.sampleRate     = 16000;
    devSpec.numChannel     = 1;
    devSpec.bytesPerSample = 2;
    devSpec.bitsPerSample  = 16;
    devSpec.format         = AudioFormatS16;
    return devSpec;
}

} // namespace

TEST_SUITE("MusicPlayList Pending Switch")
{
    // ADR 0020 决策 1/2：超窗后意图已接受，commit 由 IO 结果回投触发；TC 只在
    // commit 时发出，等待期当前任务与选中项保持旧值。
    TEST_CASE("pending switch commits via the control queue and publishes selection once")
    {
        TempMediaDir temp("pendCommit");
        const std::vector<char> tone = makeToneWavBytes(1);
        REQUIRE(writeBytes(temp.file("t0.wav"), tone));
        REQUIRE(writeBytes(temp.file("t1.wav"), tone));

        SwitchRecordingCallback callback;
        WorkQueue workQueue;
        AudioSpec devSpec = pendingTestSpec();
        std::shared_ptr<MusicPlayList> playlist
            = std::make_shared<MusicPlayList>(&callback, &workQueue, devSpec);
        REQUIRE(playlist->addTrackSync(temp.file("t0.wav")));
        REQUIRE(playlist->addTrackSync(temp.file("t1.wav")));
        REQUIRE(playlist->setCurrentIndexSync(0));
        const size_t tcBaseline = callback.snapshotIndexes().size();
        REQUIRE(callback.snapshotIndexes().back() == 0);

        ScopedOpenDelay delay(300);
        CHECK(playlist->nextSync() == true);
        CHECK(playlist->switchPending() == true);
        // 等待期不发布目标曲：TC 计数不变、当前任务仍是旧曲。
        CHECK(callback.snapshotIndexes().size() == tcBaseline);
        PlaybackTaskPtr duringWait = playlist->currentTask();
        REQUIRE(duringWait != nullptr);
        CHECK(duringWait->trackUid == playlist->trackUidForTest(0));

        // commit 后恰好一次 TC、指向新曲、任务已接管。
        REQUIRE(waitFor([&]() { return callback.snapshotIndexes().size() == tcBaseline + 1; },
                        2000));
        CHECK(playlist->switchPending() == false);
        CHECK(callback.snapshotIndexes().back() == 1);
        PlaybackTaskPtr committed = playlist->currentTask();
        REQUIRE(committed != nullptr);
        CHECK(committed->trackUid == playlist->trackUidForTest(1));
        // 一次命令一个最终结果：settlement 恰好一次 Committed（User）。
        REQUIRE(callback.settlementCount() == 1);
        CHECK(callback.settlements.front() == SwitchSettlement::Committed);
        CHECK(callback.settlementOrigins.front() == SwitchOrigin::User);
    }

    // ADR 0020 决策 4 / #276：pending 重开失败只报一次错、不改播放状态、旧任务继续。
    TEST_CASE("pending open failure reports once and keeps the old task")
    {
        TempMediaDir temp("pendFail");
        const std::vector<char> tone = makeToneWavBytes(1);
        const std::string t1 = temp.file("t1.wav");
        REQUIRE(writeBytes(temp.file("t0.wav"), tone));
        REQUIRE(writeBytes(t1, tone));

        SwitchRecordingCallback callback;
        WorkQueue workQueue;
        AudioSpec devSpec = pendingTestSpec();
        std::shared_ptr<MusicPlayList> playlist
            = std::make_shared<MusicPlayList>(&callback, &workQueue, devSpec);
        REQUIRE(playlist->addTrackSync(temp.file("t0.wav")));
        REQUIRE(playlist->addTrackSync(t1));
        REQUIRE(playlist->setCurrentIndexSync(0));
        const size_t tcBaseline = callback.snapshotIndexes().size();

        // 目标文件在入列之后、重开之前被删：open 在延迟之后失败。
        REQUIRE(std::remove(t1.c_str()) == 0);
        ScopedOpenDelay delay(300);
        CHECK(playlist->nextSync() == true);
        CHECK(playlist->switchPending() == true);

        REQUIRE(waitFor([&]() { return callback.openFailureCount() == 1; }, 2000));
        CHECK(callback.openFailures.front() == ErrorCode::FileOpenFailed);
        CHECK(callback.openFailureIndexes.front() == 1);
        REQUIRE(callback.settlementCount() == 1);
        CHECK(callback.settlements.front() == SwitchSettlement::Failed);
        CHECK(callback.settlementOrigins.front() == SwitchOrigin::User);
        CHECK(playlist->switchPending() == false);
        // 失败不改当前曲目：TC 计数不变，旧任务还活着（设备语义由播放器层保证）。
        CHECK(callback.snapshotIndexes().size() == tcBaseline);
        PlaybackTaskPtr task = playlist->currentTask();
        REQUIRE(task != nullptr);
        CHECK(task->trackUid == playlist->trackUidForTest(0));
    }

    // ADR 0020 决策 3：stop 作废在途切换；晚到的 IO 结果被代际守卫静默丢弃。
    TEST_CASE("stop cancels a pending switch and the late result is dropped")
    {
        TempMediaDir temp("pendStop");
        const std::vector<char> tone = makeToneWavBytes(1);
        REQUIRE(writeBytes(temp.file("t0.wav"), tone));
        REQUIRE(writeBytes(temp.file("t1.wav"), tone));

        SwitchRecordingCallback callback;
        WorkQueue workQueue;
        AudioSpec devSpec = pendingTestSpec();
        std::shared_ptr<MusicPlayList> playlist
            = std::make_shared<MusicPlayList>(&callback, &workQueue, devSpec);
        REQUIRE(playlist->addTrackSync(temp.file("t0.wav")));
        REQUIRE(playlist->addTrackSync(temp.file("t1.wav")));
        REQUIRE(playlist->setCurrentIndexSync(0));
        const size_t tcBaseline = callback.snapshotIndexes().size();

        ScopedOpenDelay delay(300);
        CHECK(playlist->nextSync() == true);
        CHECK(playlist->switchPending() == true);
        playlist->stopCurrentTask();
        CHECK(playlist->switchPending() == false);
        REQUIRE(callback.settlementCount() == 1);
        CHECK(callback.settlements.front() == SwitchSettlement::Cancelled);
        CHECK(callback.settlementOrigins.front() == SwitchOrigin::User);

        // 300 ms 注入的 open 返回后，晚到的结果不得再建任务、不得发布 TC。
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        CHECK(callback.snapshotIndexes().size() == tcBaseline);
        CHECK(callback.settlementCount() == 1);
        CHECK(playlist->currentTask() == nullptr);
    }

    // ADR 0020 决策 3：再次切歌只保留最新目标；被顶替的 pending 不产生终态。
    TEST_CASE("a newer switch supersedes the pending one")
    {
        TempMediaDir temp("pendSup");
        const std::vector<char> tone = makeToneWavBytes(1);
        REQUIRE(writeBytes(temp.file("t0.wav"), tone));
        REQUIRE(writeBytes(temp.file("t1.wav"), tone));
        REQUIRE(writeBytes(temp.file("t2.wav"), tone));

        SwitchRecordingCallback callback;
        WorkQueue workQueue;
        AudioSpec devSpec = pendingTestSpec();
        std::shared_ptr<MusicPlayList> playlist
            = std::make_shared<MusicPlayList>(&callback, &workQueue, devSpec);
        REQUIRE(playlist->addTrackSync(temp.file("t0.wav")));
        REQUIRE(playlist->addTrackSync(temp.file("t1.wav")));
        REQUIRE(playlist->addTrackSync(temp.file("t2.wav")));
        REQUIRE(playlist->setCurrentIndexSync(0));
        // 注意：addTrackSync 首条入列会发布 TC（空列表首发设当前），
        // setCurrentIndexSync(0) 又发一次；基线必须取在 pending 开始之前。
        const size_t tcBaseline = callback.snapshotIndexes().size();

        ScopedOpenDelay delay(300);
        CHECK(playlist->nextSync() == true); // 目标 t1：pending
        CHECK(playlist->switchPending() == true);
        // 执行器串行：t1 的在途 open 无法取消，t2 的请求在它返回后才开始服务。
        CHECK(playlist->setCurrentIndexSync(2) == true);
        CHECK(playlist->switchPending() == true);

        REQUIRE(waitFor([&]() {
            return callback.snapshotIndexes().size() == tcBaseline + 1;
        }, 3000));
        CHECK(playlist->switchPending() == false);
        CHECK(callback.snapshotIndexes().back() == 2);
        // 一次命令一个最终结果：被顶替者静默，只有 t2 的 Committed。
        REQUIRE(callback.settlementCount() == 1);
        CHECK(callback.settlements.front() == SwitchSettlement::Committed);
        CHECK(callback.openFailureCount() == 0);
    }

    // ADR 0020 决策 3：删除 pending 目标作废切换，晚到结果丢弃。
    TEST_CASE("removing the pending target cancels the switch")
    {
        TempMediaDir temp("pendRm");
        const std::vector<char> tone = makeToneWavBytes(1);
        const std::string t1 = temp.file("t1.wav");
        REQUIRE(writeBytes(temp.file("t0.wav"), tone));
        REQUIRE(writeBytes(t1, tone));

        SwitchRecordingCallback callback;
        WorkQueue workQueue;
        AudioSpec devSpec = pendingTestSpec();
        std::shared_ptr<MusicPlayList> playlist
            = std::make_shared<MusicPlayList>(&callback, &workQueue, devSpec);
        REQUIRE(playlist->addTrackSync(temp.file("t0.wav")));
        REQUIRE(playlist->addTrackSync(t1));
        REQUIRE(playlist->setCurrentIndexSync(0));
        const size_t tcBaseline = callback.snapshotIndexes().size();

        ScopedOpenDelay delay(300);
        CHECK(playlist->nextSync() == true);
        CHECK(playlist->switchPending() == true);
        CHECK(playlist->removeTrackSync(1) == PlaylistEditOutcome::RemovedOther);
        CHECK(playlist->switchPending() == false);
        REQUIRE(callback.settlementCount() == 1);
        CHECK(callback.settlements.front() == SwitchSettlement::Cancelled);

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        CHECK(callback.snapshotIndexes().size() == tcBaseline);
        CHECK(playlist->currentTask() != nullptr);
        CHECK(playlist->currentTask()->trackUid == playlist->trackUidForTest(0));
    }

    // ADR 0018 §3 + ADR 0020：补位 pending 的目标再被删时，作废并由本次删除的
    // 补位序重新落点——「列表非空必有当前曲目」不因作废而破。
    TEST_CASE("cancelling a seat-replacement pending re-seats a new current track")
    {
        TempMediaDir temp("pendSeat");
        const std::vector<char> tone = makeToneWavBytes(1);
        REQUIRE(writeBytes(temp.file("t0.wav"), tone));
        REQUIRE(writeBytes(temp.file("t1.wav"), tone));
        REQUIRE(writeBytes(temp.file("t2.wav"), tone));

        SwitchRecordingCallback callback;
        WorkQueue workQueue;
        AudioSpec devSpec = pendingTestSpec();
        std::shared_ptr<MusicPlayList> playlist
            = std::make_shared<MusicPlayList>(&callback, &workQueue, devSpec);
        REQUIRE(playlist->addTrackSync(temp.file("t0.wav")));
        REQUIRE(playlist->addTrackSync(temp.file("t1.wav")));
        REQUIRE(playlist->addTrackSync(temp.file("t2.wav")));
        REQUIRE(playlist->setCurrentIndexSync(0));
        const size_t tcBaseline = callback.snapshotIndexes().size();

        ScopedOpenDelay delay(300);
        // 删当前曲目 → 补位 pending 到 t1（压缩后 t1 在 index 0）。
        CHECK(playlist->removeTrackSync(0) == PlaylistEditOutcome::CurrentReplaced);
        CHECK(playlist->switchPending() == true);
        // 再删补位目标（现在在 index 0）→ 作废 + 重新补位到 t2（压缩后 index 0）。
        CHECK(playlist->removeTrackSync(0) == PlaylistEditOutcome::CurrentReplaced);
        CHECK(playlist->switchPending() == true);

        REQUIRE(waitFor([&]() {
            return callback.snapshotIndexes().size() == tcBaseline + 1;
        }, 3000));
        CHECK(playlist->switchPending() == false);
        // 两次结算：删 P1 目标作废（Cancelled），重新补位 t2 的 pending 完成（Committed）。
        REQUIRE(callback.settlementCount() == 2);
        CHECK(callback.settlements[0] == SwitchSettlement::Cancelled);
        CHECK(callback.settlements[1] == SwitchSettlement::Committed);
        CHECK(callback.settlementOrigins[1] == SwitchOrigin::SeatReplacement);
        PlaybackTaskPtr task = playlist->currentTask();
        REQUIRE(task != nullptr);
        CHECK(task->trackUid == playlist->trackUidForTest(0));
    }

    // ADR 0020 决策 4：段末前进同样可转 pending；失败终态归属不变。
    TEST_CASE("end-of-stream advance may go pending and stop cancels it")
    {
        TempMediaDir temp("pendEos");
        const std::vector<char> tone = makeToneWavBytes(1);
        REQUIRE(writeBytes(temp.file("t0.wav"), tone));
        REQUIRE(writeBytes(temp.file("t1.wav"), tone));

        SwitchRecordingCallback callback;
        WorkQueue workQueue;
        AudioSpec devSpec = pendingTestSpec();
        std::shared_ptr<MusicPlayList> playlist
            = std::make_shared<MusicPlayList>(&callback, &workQueue, devSpec);
        REQUIRE(playlist->addTrackSync(temp.file("t0.wav")));
        REQUIRE(playlist->addTrackSync(temp.file("t1.wav")));
        REQUIRE(playlist->setCurrentIndexSync(0));

        ScopedOpenDelay delay(300);
        CHECK(playlist->advanceToNextTrack() == true);
        CHECK(playlist->switchPending() == true);
        playlist->stopCurrentTask();
        REQUIRE(callback.settlementCount() == 1);
        CHECK(callback.settlements.front() == SwitchSettlement::Cancelled);
        CHECK(callback.settlementOrigins.front() == SwitchOrigin::EndOfStream);
    }

    // ADR 0020 决策 4：候选循环在 pending 失败后从下一个候选继续；中间候选只
    // 报一次错，一次命令一个最终结果（切到 t2）。
    TEST_CASE("auto-skip continues from the next candidate after a pending failure")
    {
        TempMediaDir temp("pendSkip");
        const std::vector<char> tone = makeToneWavBytes(1);
        const std::string t1 = temp.file("t1.wav");
        REQUIRE(writeBytes(temp.file("t0.wav"), tone));
        REQUIRE(writeBytes(t1, tone));
        REQUIRE(writeBytes(temp.file("t2.wav"), tone));

        SwitchRecordingCallback callback;
        WorkQueue workQueue;
        AudioSpec devSpec = pendingTestSpec();
        std::shared_ptr<MusicPlayList> playlist
            = std::make_shared<MusicPlayList>(&callback, &workQueue, devSpec);
        REQUIRE(playlist->addTrackSync(temp.file("t0.wav")));
        REQUIRE(playlist->addTrackSync(t1));
        REQUIRE(playlist->addTrackSync(temp.file("t2.wav")));
        REQUIRE(playlist->setCurrentIndexSync(0));

        REQUIRE(std::remove(t1.c_str()) == 0);
        ScopedOpenDelay delay(300);
        CHECK(playlist->skipToNextPlayable() == true); // t1：pending
        CHECK(playlist->switchPending() == true);

        // t1 失败（300 ms 后）→ 从 t2 续做 → t2 也转 pending（执行器串行）→
        // 再 300 ms 后 commit。总计约 600 ms，判据超时给 4 s。
        REQUIRE(waitFor([&]() {
            const std::vector<int> idx = callback.snapshotIndexes();
            return idx.size() >= 2 && idx.back() == 2;
        }, 4000));
        CHECK(playlist->switchPending() == false);
        // 中间候选恰好一次 FileOpenFailed；终态只有一个 Committed。
        REQUIRE(callback.openFailureCount() == 1);
        CHECK(callback.openFailures.front() == ErrorCode::FileOpenFailed);
        CHECK(callback.openFailureIndexes.front() == 1);
        REQUIRE(callback.settlementCount() == 1);
        CHECK(callback.settlements.front() == SwitchSettlement::Committed);
        CHECK(callback.settlementOrigins.front() == SwitchOrigin::AutoSkip);
        PlaybackTaskPtr task = playlist->currentTask();
        REQUIRE(task != nullptr);
        CHECK(task->trackUid == playlist->trackUidForTest(2));
    }
}

TEST_SUITE("MusicPlayer Pending Switch")
{
    // ADR 0020 决策 3：等待期 play 只记「就绪后起播」意图，不作用于旧任务、
    // 不取消切换；commit 时按 _play() 既有路径起播新曲。
    TEST_CASE("play during a pending switch starts the committed track")
    {
        TempMediaDir temp("pendPlay");
        const std::vector<char> tone = makeToneWavBytes(2);
        REQUIRE(writeBytes(temp.file("first.wav"), tone));
        REQUIRE(writeBytes(temp.file("second.wav"), tone));

        MockMusicPlayerListener listener;
        std::string logDir = "./log";
        MusicPlayer player(&listener, logDir);
        player.addMusicDir(temp.dir());
        REQUIRE(waitFor([&player]() { return player.getMusicCount() == 2; }, 5000));
        player.setCurrentIndex(0);
        REQUIRE(waitFor([&listener]() { return listener.last_current_index.load() == 0; }, 2000));
        player.play();
        REQUIRE(waitForState(player, MusicPlayerState::PlayingState));

        ScopedOpenDelay delay(400);
        // next 与 play 同在一条控制队列上按序执行：_next 先接受 pending（等待期
        // 最多 50 ms 占住队列），随后 play() 命中等待期 → 只记意图。
        player.next();
        player.play();

        // commit（400 ms 后）→ TC 指向新曲 → 兑现意图起播。
        REQUIRE(waitFor([&listener]() { return listener.last_current_index.load() == 1; }, 4000));
        REQUIRE(waitForState(player, MusicPlayerState::PlayingState));
        // 意图只兑现一次：状态稳定在 Playing、无错误。
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(player.state() == MusicPlayerState::PlayingState);
        CHECK(listener.error_count.load() == 0);
    }
}
