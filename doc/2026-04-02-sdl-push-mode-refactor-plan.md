# SDL 音频接口 Push 模式重构实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 SDL 音频播放从 callback 被动模式改为 push 主动模式，移除 AudioDataCallback 接口，使用 write() 主动写入

**Architecture:** AudioDevice 改用 SDL_OpenAudioDevice + SDL_QueueAudio；MusicPlayer 使用 WorkQueue 定时任务作为消费线程，从 RingBuffer 读取数据写入设备

**Tech Stack:** SDL2, C++11, WorkQueue (cosmos/)

---

## 文件结构

| 文件 | 职责 |
|------|------|
| `sdk/audio/device/AudioDevice.h` | 新增 write/clearQueue/getQueuedBytes 接口 |
| `sdk/audio/device/AudioDevice.cpp` | 实现 push 模式 SDL 调用 |
| `sdk/audio/device/AudioDevice.h` (修改) | 移除 AudioDataCallback 依赖 |
| `sdk/MusicPlayer.cpp` | 实现消费线程逻辑 |
| `sdk/test/unit/test_audio_device.cpp` | 修改/新增测试 |

---

## Task 1: AudioDevice 新增 Push 模式接口

**Files:**
- Modify: `sdk/audio/device/AudioDevice.h:8-12` (移除 AudioDataCallback)
- Modify: `sdk/audio/device/AudioDevice.h:27-37` (新增接口)
- Modify: `sdk/audio/device/AudioDevice.cpp:1-42` (实现改动)

- [ ] **Step 1: 修改 AudioDevice.h - 移除 AudioDataCallback 依赖**

```cpp
// 删除以下内容:
/*
class AudioDataCallback {
public:
    virtual ~AudioDataCallback()                   = default;
    virtual void getAudioData(void *data, int len) = 0;
};
*/

// 修改构造函数: 从 AudioDataCallback* 改为空
// 旧: AudioDevice(AudioDataCallback *callback);
// 新: AudioDevice();
```

- [ ] **Step 2: 修改 AudioDevice.h - 新增 Push 模式接口**

在 public 部分添加:
```cpp
class AudioDevice : public NonCopyable {
public:
    AudioDevice();  // 无参数构造
    ~AudioDevice();
    
    // Push 模式接口
    int write(const void* data, size_t len);
    size_t getQueuedBytes() const;
    void clearQueue();
    
    // ... 保留原有接口
    std::vector<AudDevPair> getDeviceList();
    int getDeviceSpec(AudioSpec &spec);
    int selectDevice(uint64_t id);
    int open();
    int close();
    int start();
    int stop();
};
```

- [ ] **Step 3: 修改 AudioDevice.cpp - 实现 Push 模式**

```cpp
// 1. 修改 Impl 成员
class AudioDevice::Impl {
private:
    SDL_AudioDeviceID m_deviceID;  // 新增: 设备 ID
    // 移除: AudioDataCallback *m_callback;
    
public:
    Impl();  // 无参数构造
    // 移除: Impl(AudioDataCallback *callback);
    
    int write(const void* data, size_t len);
    size_t getQueuedBytes() const;
    void clearQueue();
};

// 2. 修改构造函数
AudioDevice::Impl::Impl()
    : m_deviceID(0)
    , m_isOpen(false)
    , m_isStart(false)
{
    // ... 保留 SDL_Init 等初始化 ...
    // 关键: callback 设为 NULL
    m_sdlSpec.callback = NULL;
    m_sdlSpec.userdata = nullptr;
}

// 3. 修改 open() 使用 SDL_OpenAudioDevice
int AudioDevice::Impl::open() {
    if (m_isOpen) {
        LOG_ERROR(LOG_TAG, "Audio device is already open");
        return -1;
    }
    
    // 使用 SDL_OpenAudioDevice 代替 SDL_OpenAudio
    SDL_AudioSpec obtained;
    m_deviceID = SDL_OpenAudioDevice(
        nullptr,           // 默认设备
        0,                 // 不是录音
        &m_sdlSpec,
        &obtained,
        0                  // flags
    );
    
    if (m_deviceID == 0) {
        LOG_ERROR(LOG_TAG, "SDL_OpenAudioDevice failed: %s", SDL_GetError());
        return -1;
    }
    
    // 更新实际 spec
    m_sdlSpec = obtained;
    m_isOpen = true;
    return 0;
}

// 4. 实现 write/clearQueue/getQueuedBytes
int AudioDevice::Impl::write(const void* data, size_t len) {
    if (!m_isOpen) {
        return -1;
    }
    int ret = SDL_QueueAudio(m_deviceID, data, len);
    if (ret != 0) {
        LOG_ERROR(LOG_TAG, "SDL_QueueAudio failed: %s", SDL_GetError());
    }
    return ret;
}

size_t AudioDevice::Impl::getQueuedBytes() const {
    if (!m_isOpen) {
        return 0;
    }
    return SDL_GetQueuedAudioSize(m_deviceID);
}

void AudioDevice::Impl::clearQueue() {
    if (m_isOpen && m_deviceID != 0) {
        SDL_ClearQueuedAudio(m_deviceID);
    }
}

// 5. 修改 close()
void AudioDevice::Impl::close() {
    if (!m_isOpen) {
        return;
    }
    if (m_deviceID != 0) {
        SDL_CloseAudioDevice(m_deviceID);
        m_deviceID = 0;
    }
    m_isOpen = false;
}

// 6. 修改 start/stop 使用新 API
void AudioDevice::Impl::start() {
    if (m_isStart || !m_isOpen) {
        return;
    }
    SDL_PauseAudioDevice(m_deviceID, 0);  // 0 = 不暂停(播放)
    m_isStart = true;
}

void AudioDevice::Impl::stop() {
    if (!m_isStart || !m_isOpen) {
        return;
    }
    SDL_PauseAudioDevice(m_deviceID, 1);  // 1 = 暂停
    m_isStart = false;
}
```

- [ ] **Step 4: 修改 AudioDevice 构造函数**

```cpp
// AudioDevice.h 中
AudioDevice::AudioDevice()
    : m_impl(new Impl())
{
}

// 删除旧的 AudioDevice(AudioDataCallback *callback)
```

- [ ] **Step 5: 添加新接口的外部封装**

```cpp
// AudioDevice.cpp 末尾添加
int AudioDevice::write(const void* data, size_t len) {
    return m_impl->write(data, len);
}

size_t AudioDevice::getQueuedBytes() const {
    return m_impl->getQueuedBytes();
}

void AudioDevice::clearQueue() {
    m_impl->clearQueue();
}
```

- [ ] **Step 6: 运行 lsp_diagnostics 验证**

```
Run: lsp_diagnostics on sdk/audio/device/
Expected: 无编译错误
```

---

## Task 2: MusicPlayer 移除 AudioDataCallback 实现消费线程

**Files:**
- Modify: `sdk/MusicPlayer.h:19` (移除 AudioDataCallback 继承)
- Modify: `sdk/MusicPlayer.cpp:19` (移除 AudioDataCallback 继承)
- Modify: `sdk/MusicPlayer.cpp:52` (移除 getAudioData 声明)
- Modify: `sdk/MusicPlayer.cpp:106` (修改构造)
- Modify: `sdk/MusicPlayer.cpp:396-455` (移除 getAudioData 实现，新增消费逻辑)
- Modify: `sdk/MusicPlayer.cpp:64` (新增消费线程辅助函数声明)

- [ ] **Step 1: 修改 MusicPlayer.h - 移除 AudioDataCallback**

```cpp
// 旧:
class MusicPlayer::Impl : public AudioDataCallback, public MusicPlayListCallback {

// 新:
class MusicPlayer::Impl : public MusicPlayListCallback {
```

- [ ] **Step 2: 修改 MusicPlayer.cpp - 移除 AudioDataCallback 继承**

```cpp
// 约第 19 行
class MusicPlayer::Impl : public MusicPlayListCallback {
```

- [ ] **Step 3: 移除 getAudioData 相关声明和实现**

删除约第 52 行:
```cpp
protected:
    virtual void getAudioData(void *data, int len);
```

删除约第 396-455 行的实现:
```cpp
void MusicPlayer::Impl::getAudioData(void *data, int len)
{
    // ... 整个函数体
}
```

- [ ] **Step 4: 在 MusicPlayer::Impl 中新增消费线程逻辑**

在 private 部分添加:
```cpp
private:
    // ... 现有成员 ...
    
    // 新增消费线程相关
    WorkQueue::TaskID m_consumerTaskID;
    void startConsumerThread();
    void stopConsumerThread();
    void consumeAudioData();
    bool shouldFillSilence() const;
```

实现新增的函数:
```cpp
void MusicPlayer::Impl::startConsumerThread() {
    if (m_consumerTaskID != WorkQueue::TaskID_Error) {
        return;  // 已运行
    }
    m_consumerTaskID = m_workQueue.startTimerTask(
        true,  // repeat
        10,    // 10ms interval
        [this]() { this->consumeAudioData(); }
    );
    LOG_INFO(LOG_TAG, "Consumer thread started, taskID=%zu", m_consumerTaskID);
}

void MusicPlayer::Impl::stopConsumerThread() {
    if (m_consumerTaskID == WorkQueue::TaskID_Error) {
        return;
    }
    m_workQueue.stopTimerTask(m_consumerTaskID);
    m_consumerTaskID = WorkQueue::TaskID_Error;
    LOG_INFO(LOG_TAG, "Consumer thread stopped");
}

bool MusicPlayer::Impl::shouldFillSilence() const {
    if (isShuttingDown() || m_switching.load()) {
        return true;
    }
    MusicPropertiesPtr cur;
    {
        std::lock_guard<std::mutex> lock(m_curMusicMutex);
        cur = m_curMusicProperties;
    }
    if (cur == nullptr || cur->ringBuffer == nullptr || cur->streamDecoder == nullptr) {
        return true;
    }
    if (cur->streamDecoder->state() == StreamDecoderState::SEEKING) {
        return true;
    }
    return false;
}

void MusicPlayer::Impl::consumeAudioData() {
    if (shouldFillSilence()) {
        // 生成静音
        const size_t len = 4096;  // 4KB
        std::vector<char> silence(len, 0);
        m_audioDev->write(silence.data(), len);
        return;
    }
    
    MusicPropertiesPtr cur;
    {
        std::lock_guard<std::mutex> lock(m_curMusicMutex);
        cur = m_curMusicProperties;
    }
    
    AudioRingBuffer* ring = cur->ringBuffer.get();
    
    // 检查 SDL 队列是否过满 (阈值: 256KB)
    const size_t kQueueThreshold = 256 * 1024;
    if (m_audioDev->getQueuedBytes() > kQueueThreshold) {
        // 队列太满，跳过本次写入
        return;
    }
    
    // 从 ring buffer 读取
    const size_t kChunkSize = 4096;
    std::vector<char> buf(kChunkSize);
    size_t got = ring->read(buf.data(), kChunkSize);
    
    if (got > 0) {
        m_audioDev->write(buf.data(), got);
        
        // 更新播放位置 (与原 getAudioData 逻辑相同)
        SignalProperties& signalProperties = cur->signalProperties;
        AudioSpec& spec = m_devSpec;
        signalProperties.curDataOffset.fetch_add(static_cast<off64_t>(got), std::memory_order_relaxed);
        uint64_t consumed = static_cast<uint64_t>(signalProperties.curDataOffset.load());
        uint64_t bytesPerMs = static_cast<uint64_t>(spec.sampleRate)
            * static_cast<uint64_t>(spec.numChannel) * static_cast<uint64_t>(spec.bytesPerSample)
            / 1000;
        signalProperties.curPositionMs = (bytesPerMs > 0) ? (consumed / bytesPerMs) : 0;
        
        uint64_t curPositionMs = signalProperties.curPositionMs;
        if (!isShuttingDown()) {
            m_workQueue.asyncRunTask([this, curPositionMs]() {
                if (!isShuttingDown()) {
                    m_listener->onMusicPlayerPositionChanged(curPositionMs);
                }
            });
        }
        
        // 检查 EOS
        StreamDecoderState st = cur->streamDecoder->state();
        if (st == StreamDecoderState::EOS && ring->availableRead() == 0) {
            if (!isShuttingDown() && !m_switching.load() && !m_switching.exchange(true)) {
                m_workQueue.asyncRunTask([this]() {
                    if (!isShuttingDown()) {
                        if (!m_musicList->advanceToNextTrack()) {
                            _stop();
                        }
                    }
                    m_switching.store(false);
                });
            }
        }
    }
}
```

- [ ] **Step 5: 修改构造函数 - 初始化消费者任务 ID**

```cpp
// 约第 85-113 行，构造函数中
MusicPlayer::Impl::Impl(MusicPlayerListener* lister, std::string& logDir)
    : m_audioDev(nullptr)
    , m_musicList(nullptr)
    , m_listener(lister)
    , m_state(MusicPlayerState::StoppedState)
    , m_pendingAdd(0)
    , m_traceSeq(0)
    , m_autoSkipOnError(true)
    , m_skipFailures(0)
    , m_switching(false)
    , m_shuttingDown(false)
    , m_workQueue()
    , m_consumerTaskID(WorkQueue::TaskID_Error)  // 新增
{
    // ... 现有初始化 ...
    m_audioDev = std::make_shared<AudioDevice>();  // 改为无参数构造
    // ... 后续不变 ...
}
```

- [ ] **Step 6: 修改 _play() 启动消费线程**

```cpp
// 约在 play() 相关函数中，找到 play 逻辑
void MusicPlayer::Impl::_play() {
    // ... 现有代码 ...
    
    // 启动消费线程
    startConsumerThread();
}
```

- [ ] **Step 7: 修改 _stop() 停止消费线程**

```cpp
// 约在 stop 相关函数
void MusicPlayer::Impl::_stop() {
    // 停止消费线程
    stopConsumerThread();
    
    // 清空队列
    if (m_audioDev) {
        m_audioDev->clearQueue();
    }
    
    // ... 现有代码 ...
}
```

- [ ] **Step 8: 修改析构函数**

```cpp
// 约第 115-123 行
MusicPlayer::Impl::~Impl() {
    m_shuttingDown.store(true);
    m_switching.store(true);
    
    // 新增: 先停止消费线程
    stopConsumerThread();
    
    if (m_audioDev != nullptr) {
        m_audioDev->stop();
        m_audioDev->close();
    }
}
```

- [ ] **Step 9: 修改切换歌曲逻辑**

在切歌时需要清空队列:
```cpp
// 在需要切换歌曲的地方 (如 _next, _previous, _setCurrentIndex)
// 在切换前调用
m_audioDev->clearQueue();
```

- [ ] **Step 10: 运行 lsp_diagnostics 验证**

```
Run: lsp_diagnostics on sdk/MusicPlayer.cpp
Expected: 无编译错误
```

---

## Task 3: 更新测试用例

**Files:**
- Modify: `sdk/test/unit/test_audio_device.cpp`
- Modify: `sdk/test/TestAudioDevice.cpp`

- [ ] **Step 1: 修改 test_audio_device.cpp - 测试 push 模式**

```cpp
// 修改现有测试或新增测试

// 1. 测试 write 接口
TEST_CASE("AudioDevice write") {
    AudioDevice dev;  // 无参数构造
    
    // open/start
    REQUIRE(dev.open() == 0);
    REQUIRE(dev.start() == 0);
    
    // 写入测试数据
    std::vector<uint8_t> testData(1024, 0x00);
    REQUIRE(dev.write(testData.data(), testData.size()) == 0);
    
    // 验证队列有数据
    size_t queued = dev.getQueuedBytes();
    CHECK(queued > 0);
    
    // 清理
    dev.clearQueue();
    dev.stop();
    dev.close();
}

// 2. 测试 clearQueue
TEST_CASE("AudioDevice clearQueue") {
    AudioDevice dev;
    REQUIRE(dev.open() == 0);
    REQUIRE(dev.start() == 0);
    
    std::vector<uint8_t> data(512, 0x00);
    dev.write(data.data(), data.size());
    
    // 清空队列
    dev.clearQueue();
    
    size_t queued = dev.getQueuedBytes();
    CHECK(queued == 0);
    
    dev.stop();
    dev.close();
}
```

- [ ] **Step 2: 修改 TestAudioDevice.cpp - 如有需要**

根据现有测试内容调整

- [ ] **Step 3: 运行测试验证**

```
Run: ctest --test-dir build -R sdk_unit_tests --output-on-failure
Expected: 测试通过
```

---

## Task 4: 编译与回归测试

- [ ] **Step 1: 编译整个项目**

```bash
cd build
cmake --build . --target SoundBridge 2>&1 | head -50
```

- [ ] **Step 2: 编译测试**

```bash
cmake --build . --target TestSdkSuite
cmake --build . --target UnitTests
```

- [ ] **Step 3: 运行单元测试**

```bash
ctest --test-dir build -R sdk_unit_tests --output-on-failure
```

- [ ] **Step 4: 如有需要，手动测试播放功能**

---

## 执行方式选择

**Plan complete and saved to `doc/2026-04-02-sdl-push-mode-refactor-plan.md`.**

**Two execution options:**

1. **Subagent-Driven (recommended)** - I dispatch a fresh subagent per task, review between tasks, fast iteration

2. **Inline Execution** - Execute tasks in this session using executing-plans, batch execution with checkpoints

**Which approach?**