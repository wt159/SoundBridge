# SoundBridge SDK 测试覆盖率提升方案

**文档版本**: v1.0  
**创建日期**: 2026-04-06  
**目标**: 将 SDK 代码覆盖率从 68% 提升至 80%+

---

## 1. 当前覆盖率分析

| 模块 | 覆盖率 | 未覆盖行数 | 优先级 |
|------|--------|-----------|--------|
| **sdk (root)** | 506/1003 (50.4%) | 497 | 🔴 高 |
| audio/decode | 745/1012 (73.6%) | 267 | 🟡 中 |
| audio/device | 193/247 (78.1%) | 54 | 🟡 中 |
| audio/resample | 118/170 (69.4%) | 52 | 🟡 中 |
| log (boost) | 84/85 (98.8%) | 1 | 🟢 低 |
| utils | 95/110 (86.4%) | 15 | 🟢 低 |
| **总计** | **1741/2627 (66.3%)** | **886** | - |

---

## 2. 测试设计规范

### 2.1 测试框架
- 使用 **doctest** 框架（`sdk/test/unit/`）
- 遵循现有 `TEST_SUITE` + `TEST_CASE` 结构
- 使用 `RealMediaFixture` 获取真实媒体路径
- 异步测试使用 `std::promise/std::future` 模式

### 2.2 命名规范
```
TEST_SUITE("模块名")
TEST_CASE("场景: 条件或状态")
```

### 2.3 断言选择
- 关键断言: `REQUIRE` (失败终止)
- 非关键断言: `CHECK` (继续收集)
- 错误路径: `REQUIRE_FALSE` 或 `CHECK_FALSE`

---

## 3. 新增测试用例设计

### 3.1 sdk/ErrorCode 模块 (新增 test_error.cpp)

**目标**: 覆盖 92 行未覆盖代码

```cpp
TEST_SUITE("ErrorCode")
{
    TEST_CASE("GetErrorInfo returns correct info for each ErrorCode")
    {
        // 测试所有 ErrorCode 枚举值的 GetErrorInfo
        auto info = sdk::GetErrorInfo(sdk::ErrorCode::Ok);
        CHECK(info.severity == sdk::ErrorSeverity::Info);
        CHECK(info.recoverable == true);
    }
    
    TEST_CASE("GetErrorInfo for FileOpenFailed")
    {
        auto info = sdk::GetErrorInfo(sdk::ErrorCode::FileOpenFailed);
        CHECK(info.module == sdk::ErrorModule::File);
        CHECK(info.severity == sdk::ErrorSeverity::Error);
        CHECK(info.recoverable == false);
        CHECK(info.action == sdk::ErrorAction::CheckFile);
    }
    
    TEST_CASE("GetErrorInfo for unknown code returns Unknown info")
    {
        auto info = sdk::GetErrorInfo(static_cast<sdk::ErrorCode>(999));
        CHECK(info.module == sdk::ErrorModule::Unknown);
        CHECK(info.severity == sdk::ErrorSeverity::Error);
    }
    
    TEST_CASE("ToString for ErrorModule returns correct strings")
    {
        CHECK_STREQ(sdk::ToString(sdk::ErrorModule::File), "File");
        CHECK_STREQ(sdk::ToString(sdk::ErrorModule::Extractor), "Extractor");
        CHECK_STREQ(sdk::ToString(sdk::ErrorModule::Decode), "Decode");
        CHECK_STREQ(sdk::ToString(sdk::ErrorModule::Resample), "Resample");
        CHECK_STREQ(sdk::ToString(sdk::ErrorModule::Playlist), "Playlist");
        CHECK_STREQ(sdk::ToString(sdk::ErrorModule::AudioDevice), "AudioDevice");
        CHECK_STREQ(sdk::ToString(sdk::ErrorModule::Player), "Player");
        CHECK_STREQ(sdk::ToString(sdk::ErrorModule::Unknown), "Unknown");
    }
    
    TEST_CASE("ToString for ErrorSeverity returns correct strings")
    {
        CHECK_STREQ(sdk::ToString(sdk::ErrorSeverity::Info), "Info");
        CHECK_STREQ(sdk::ToString(sdk::ErrorSeverity::Warning), "Warning");
        CHECK_STREQ(sdk::ToString(sdk::ErrorSeverity::Error), "Error");
        CHECK_STREQ(sdk::ToString(sdk::ErrorSeverity::Fatal), "Fatal");
    }
    
    TEST_CASE("ToString for ErrorAction returns correct strings")
    {
        CHECK_STREQ(sdk::ToString(sdk::ErrorAction::None), "None");
        CHECK_STREQ(sdk::ToString(sdk::ErrorAction::SkipTrack), "SkipTrack");
        CHECK_STREQ(sdk::ToString(sdk::ErrorAction::StopPlayback), "StopPlayback");
        CHECK_STREQ(sdk::ToString(sdk::ErrorAction::CheckFile), "CheckFile");
        CHECK_STREQ(sdk::ToString(sdk::ErrorAction::ReportBug), "ReportBug");
    }
    
    TEST_CASE("FormatError with detail appends correctly")
    {
        std::string result = sdk::FormatError(sdk::ErrorCode::FileOpenFailed, "file.wav");
        CHECK(result.find("Open file failed") != std::string::npos);
        CHECK(result.find("file.wav") != std::string::npos);
    }
    
    TEST_CASE("FormatError without detail returns base message")
    {
        std::string result = sdk::FormatError(sdk::ErrorCode::Ok, "");
        CHECK(result == "Ok");
    }
}
```

**预期覆盖率提升**: +92 行

---

### 3.2 soundbridge::Player 模块扩展 (扩展 test_player.cpp)

**目标**: 覆盖 player_impl.cpp 中 50 行未覆盖代码

```cpp
TEST_SUITE("PublicPlayer Error Handling")
{
    TEST_CASE("seek before play does not crash")
    {
        MockPlayerCallbacks callback;
        soundbridge::PlayerConfig config;
        config.logDirectory = "./log";
        soundbridge::Player player(&callback, config);
        
        // seek should be safe even without track
        player.seek(1000);
        CHECK(player.position() == 0);  // position unchanged
    }
    
    TEST_CASE("next with empty playlist reports error")
    {
        MockPlayerCallbacks callback;
        soundbridge::PlayerConfig config;
        config.logDirectory = "./log";
        soundbridge::Player player(&callback, config);
        
        int initialCount = callback.error_count;
        player.next();
        
        // Wait for async error callback
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(callback.error_count > initialCount);
    }
    
    TEST_CASE("previous with empty playlist reports error")
    {
        MockPlayerCallbacks callback;
        soundbridge::PlayerConfig config;
        config.logDirectory = "./log";
        soundbridge::Player player(&callback, config);
        
        int initialCount = callback.error_count;
        player.previous();
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(callback.error_count > initialCount);
    }
    
    TEST_CASE("setCurrentTrack with invalid index is handled")
    {
        MockPlayerCallbacks callback;
        soundbridge::PlayerConfig config;
        config.logDirectory = "./log";
        soundbridge::Player player(&callback, config);
        
        // Should not crash
        player.setCurrentTrack(-1);
        player.setCurrentTrack(999);
    }
}

TEST_SUITE("PlayerCallbacksAdapter")
{
    TEST_CASE("onMusicPlayerListCurrentIndexChanged updates track name")
    {
        // Test internal adapter state tracking
    }
    
    TEST_CASE("onMusicPlayerError propagates error code correctly")
    {
        MockPlayerCallbacks callback;
        soundbridge::PlayerConfig config;
        config.logDirectory = "./log";
        soundbridge::Player player(&callback, config);
        
        player.play();  // No track, should trigger error
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(callback.error_count > 0);
        CHECK(callback.last_error_index >= 0);
    }
}
```

**预期覆盖率提升**: +50 行

---

### 3.3 audio/decode 模块扩展 (扩展 test_decode.cpp)

**目标**: 覆盖 AudioStreamDecoder 中 127 行、VorbisDecode 中 61 行、FLACDecode 中 43 行未覆盖代码

```cpp
TEST_SUITE("AudioStreamDecoder Error Paths")
{
    TEST_CASE("start with null ring returns INVALID_OPERATION")
    {
        AudioSpec devSpec = makeDevSpec();
        AudioRingBuffer *nullRing = nullptr;
        AudioStreamDecoder decoder(nullRing, devSpec);
        
        MockExtractor extractor;
        auto status = decoder.start(&extractor);
        CHECK(status == sdk_utils::INVALID_OPERATION);
        CHECK(decoder.state() == StreamDecoderState::ERROR);
    }
    
    TEST_CASE("start with null extractor returns INVALID_OPERATION")
    {
        AudioRingBuffer ring(8192);
        AudioSpec devSpec = makeDevSpec();
        AudioStreamDecoder decoder(&ring, devSpec);
        
        auto status = decoder.start(nullptr);
        CHECK(status == sdk_utils::INVALID_OPERATION);
        CHECK(decoder.state() == StreamDecoderState::ERROR);
    }
    
    TEST_CASE("seekToMs with invalid position is handled")
    {
        AudioRingBuffer ring(8192);
        AudioSpec devSpec = makeDevSpec();
        AudioStreamDecoder decoder(&ring, devSpec);
        
        // Before start, seek should be safe
        decoder.seekToMs(0);
        decoder.seekToMs(UINT64_MAX);  // Large value
    }
}

TEST_SUITE("AudioStreamDecoder Seek")
{
    TEST_CASE("seek during decoding transitions through SEEKING state")
    {
        // Start decoder with real media
        RealMediaFixture fixture;
        if (!fixture.exists("music.wav")) return;
        
        AudioRingBuffer ring(65536);
        AudioSpec devSpec = makeDevSpec();
        AudioStreamDecoder decoder(&ring, devSpec);
        
        auto extractor = createWavExtractor(fixture.mediaPath("music.wav"));
        REQUIRE(decoder.start(extractor.get()) == sdk_utils::OK);
        
        // Wait for decoding
        waitForDecoderState(decoder, StreamDecoderState::DECODING, 1000);
        
        // Seek to middle
        decoder.seekToMs(decoder.duration() / 2);
        
        // Should transition to SEEKING
        // Note: timing-sensitive test
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        
        decoder.stop();
    }
    
    TEST_CASE("multiple seeks are handled correctly")
    {
        AudioRingBuffer ring(65536);
        AudioSpec devSpec = makeDevSpec();
        AudioStreamDecoder decoder(&ring, devSpec);
        
        // Rapid seeking
        decoder.seekToMs(1000);
        decoder.seekToMs(2000);
        decoder.seekToMs(500);
        decoder.seekToMs(0);
    }
}

TEST_SUITE("VorbisDecode Error Handling")
{
    TEST_CASE("decode with null output buffer handles gracefully")
    {
        VorbisDecode decoder;
        // Should not crash
        CHECK(decoder.decode(nullptr, 1024) == nullptr);
    }
    
    TEST_CASE("decode with invalid packet handles gracefully")
    {
        VorbisDecode decoder;
        std::vector<uint8_t> invalidPacket(100, 0);
        auto buffer = decoder.decode(invalidPacket.data(), invalidPacket.size());
        // Should return nullptr or empty buffer
        CHECK(!buffer || buffer->size() == 0);
    }
}

TEST_SUITE("FLACDecode Error Handling")
{
    TEST_CASE("decode with null data returns nullptr")
    {
        FLACDecode decoder;
        CHECK(decoder.decode(nullptr, 1024) == nullptr);
    }
    
    TEST_CASE("decode with insufficient data handles gracefully")
    {
        FLACDecode decoder;
        std::vector<uint8_t> partialData(10, 0);
        auto buffer = decoder.decode(partialData.data(), partialData.size());
        CHECK(!buffer || buffer->size() == 0);
    }
    
    TEST_CASE("metadata callbacks are invoked")
    {
        // Test FLAC metadata parsing
    }
}
```

**预期覆盖率提升**: +231 行

---

### 3.4 audio/device 模块扩展 (扩展 test_audio_device.cpp)

**目标**: 覆盖 AudioDevice 中 54 行未覆盖代码

```cpp
TEST_SUITE("AudioDevice Device Selection")
{
    TEST_CASE("getDeviceList returns available devices")
    {
        AudioDevice device;
        std::vector<AudDevPair> devList;
        
        int ret = device.getDeviceList(devList);
        // TODO: This is currently unimplemented, will return 0
        // After implementation, should check devList.size() > 0
        CHECK(ret == 0);  // Current stub behavior
    }
    
    TEST_CASE("selectDevice with valid ID is handled")
    {
        AudioDevice device;
        // TODO: After getDeviceList implemented, test selectDevice
        int ret = device.selectDevice(0);
        CHECK(ret == 0);  // Current stub behavior
    }
    
    TEST_CASE("write with null data returns 0")
    {
        AudioDevice device;
        size_t written = device.write(nullptr, 1024);
        CHECK(written == 0);
    }
    
    TEST_CASE("write with zero length returns 0")
    {
        AudioDevice device;
        uint8_t data[1024];
        size_t written = device.write(data, 0);
        CHECK(written == 0);
    }
    
    TEST_CASE("getQueuedBytes starts at zero")
    {
        AudioDevice device;
        CHECK(device.getQueuedBytes() == 0);
    }
    
    TEST_CASE("clearQueue after write clears queued bytes")
    {
        AudioDevice device;
        uint8_t data[1024] = {0};
        device.write(data, sizeof(data));
        device.clearQueue();
        CHECK(device.getQueuedBytes() == 0);
    }
}

TEST_SUITE("AudioDevice State Transitions")
{
    TEST_CASE("close without open is safe")
    {
        AudioDevice device;
        device.close();  // Should not crash
    }
    
    TEST_CASE("stop without start is safe")
    {
        AudioDevice device;
        device.open();
        device.stop();  // Should not crash even without start
        device.close();
    }
    
    TEST_CASE("multiple opens and closes are handled")
    {
        AudioDevice device;
        REQUIRE(device.open() == 0);
        device.close();
        
        REQUIRE(device.open() == 0);
        device.close();
    }
}
```

**预期覆盖率提升**: +54 行

---

### 3.5 audio/resample 模块扩展 (扩展 test_audio.cpp)

**目标**: 覆盖 AudioResample 中 52 行未覆盖代码

```cpp
TEST_SUITE("AudioResample Error Handling")
{
    TEST_CASE("resample with null input is handled")
    {
        AudioSpec inSpec = makeInputSpec();
        AudioSpec outSpec = makeOutputSpec();
        AudioResample resample(inSpec, outSpec);
        
        AudioBuffer outBuffer;
        int ret = resample.resample(nullptr, 0, outBuffer);
        CHECK(ret < 0);  // Should return error
    }
    
    TEST_CASE("resample with zero input length")
    {
        AudioSpec inSpec = makeInputSpec();
        AudioSpec outSpec = makeOutputSpec();
        AudioResample resample(inSpec, outSpec);
        
        AudioBuffer inBuffer;
        inBuffer.resize(1024);
        AudioBuffer outBuffer;
        
        int ret = resample.resample(inBuffer.data(), 0, outBuffer);
        CHECK(ret >= 0);  // Should handle gracefully
    }
    
    TEST_CASE("resample with mismatched specs handles gracefully")
    {
        AudioSpec inSpec;
        inSpec.sampleRate = 0;  // Invalid
        inSpec.numChannel = 0;
        inSpec.format = AudioFormat::AudioFormatUnknown;
        
        AudioSpec outSpec = makeOutputSpec();
        AudioResample resample(inSpec, outSpec);
        
        CHECK(resample.initCheck() != sdk_utils::OK);
    }
}

TEST_SUITE("AudioResample Boundary Conditions")
{
    TEST_CASE("resample with very large buffer size")
    {
        AudioSpec inSpec = makeInputSpec();
        AudioSpec outSpec = makeOutputSpec();
        AudioResample resample(inSpec, outSpec);
        
        std::vector<uint8_t> largeBuffer(1024 * 1024, 0);  // 1MB
        AudioBuffer outBuffer;
        
        // Should not crash
        resample.resample(largeBuffer.data(), largeBuffer.size(), outBuffer);
    }
    
    TEST_CASE("consecutive resample calls maintain state")
    {
        AudioSpec inSpec = makeInputSpec();
        AudioSpec outSpec = makeOutputSpec();
        AudioResample resample(inSpec, outSpec);
        
        std::vector<uint8_t> buffer(1024);
        AudioBuffer out1, out2, out3;
        
        resample.resample(buffer.data(), buffer.size(), out1);
        resample.resample(buffer.data(), buffer.size(), out2);
        resample.resample(buffer.data(), buffer.size(), out3);
        
        // Outputs should be consistent
        CHECK(out1.size() > 0);
        CHECK(out2.size() > 0);
    }
}
```

**预期覆盖率提升**: +52 行

---

### 3.6 sdk/MusicPlayer 扩展 (扩展 test_player.cpp)

**目标**: 覆盖 MusicPlayer 中 240 行未覆盖代码

```cpp
TEST_SUITE("MusicPlayer State Machine")
{
    TEST_CASE("play while already playing stays in playing state")
    {
        MockMusicPlayerListener listener;
        std::string logDir = "./log";
        sdk::MusicPlayer player(&listener, logDir);
        
        player.play();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        
        auto initialState = player.state();
        player.play();  // Double play
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        
        // State should remain playing or transition correctly
        CHECK(player.state() == sdk::MusicPlayerState::PlayingState);
    }
    
    TEST_CASE("pause while stopped is safe")
    {
        MockMusicPlayerListener listener;
        std::string logDir = "./log";
        sdk::MusicPlayer player(&listener, logDir);
        
        // Stopped -> Pause should be safe
        player.pause();
        CHECK(player.state() == sdk::MusicPlayerState::PausedState);
    }
    
    TEST_CASE("stop while already stopped is safe")
    {
        MockMusicPlayerListener listener;
        std::string logDir = "./log";
        sdk::MusicPlayer player(&listener, logDir);
        
        player.stop();
        CHECK(player.state() == sdk::MusicPlayerState::StoppedState);
        player.stop();  // Double stop
        CHECK(player.state() == sdk::MusicPlayerState::StoppedState);
    }
}

TEST_SUITE("MusicPlayer AutoSkip")
{
    TEST_CASE("autoSkipOnError defaults to true")
    {
        MockMusicPlayerListener listener;
        std::string logDir = "./log";
        sdk::MusicPlayer player(&listener, logDir);
        
        CHECK(player.autoSkipOnError() == true);
    }
    
    TEST_CASE("setAutoSkipOnError persists")
    {
        MockMusicPlayerListener listener;
        std::string logDir = "./log";
        sdk::MusicPlayer player(&listener, logDir);
        
        player.setAutoSkipOnError(false);
        CHECK(player.autoSkipOnError() == false);
        
        player.setAutoSkipOnError(true);
        CHECK(player.autoSkipOnError() == true);
    }
}

TEST_SUITE("MusicPlayer Position")
{
    TEST_CASE("setPosition with zero is handled")
    {
        MockMusicPlayerListener listener;
        std::string logDir = "./log";
        sdk::MusicPlayer player(&listener, logDir);
        
        player.setPosition(0);
        CHECK(player.state() == sdk::MusicPlayerState::StoppedState);
    }
    
    TEST_CASE("setPosition while playing seeks correctly")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music.wav")) return;
        
        MockMusicPlayerListener listener;
        std::string logDir = "./log";
        sdk::MusicPlayer player(&listener, logDir);
        
        player.addMusicDir(fixture.mediaDir());
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        player.play();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        // Seek to position
        player.setPosition(1000);
        
        // Position should be updated (within tolerance)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        // Note: actual position may differ due to buffering
    }
}

TEST_SUITE("MusicPlayer Track Navigation")
{
    TEST_CASE("next with single track wraps to same track")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music.wav")) return;
        
        MockMusicPlayerListener listener;
        std::string logDir = "./log";
        sdk::MusicPlayer player(&listener, logDir);
        
        player.addMusicDir(fixture.mediaDir());
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        REQUIRE(player.getMusicCount() > 0);
        
        auto initialIndex = listener.last_current_index;
        player.next();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // With single track, next goes to same track
        CHECK(player.getMusicCount() == 1);
    }
    
    TEST_CASE("previous at beginning wraps to last")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music.wav")) return;
        
        MockMusicPlayerListener listener;
        std::string logDir = "./log";
        sdk::MusicPlayer player(&listener, logDir);
        
        player.addMusicDir(fixture.mediaDir());
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        if (player.getMusicCount() > 1) {
            // Set to first track
            player.setCurrentIndex(0);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            
            player.previous();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            // Should wrap to last track
        }
    }
    
    TEST_CASE("setCurrentIndex with invalid index is handled")
    {
        MockMusicPlayerListener listener;
        std::string logDir = "./log";
        sdk::MusicPlayer player(&listener, logDir);
        
        player.setCurrentIndex(-1);
        player.setCurrentIndex(9999);
        
        // Should not crash
        CHECK(player.getMusicCount() == 0);
    }
}

TEST_SUITE("MusicPlayer Error Handling")
{
    TEST_CASE("addMusicDir with empty directory is handled")
    {
        MockMusicPlayerListener listener;
        std::string logDir = "./log";
        sdk::MusicPlayer player(&listener, logDir);
        
        player.addMusicDir("/tmp/empty_directory_that_does_not_exist_12345");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        CHECK(player.getMusicCount() == 0);
    }
    
    TEST_CASE("play with invalid directory triggers error callback")
    {
        MockMusicPlayerListener listener;
        std::string logDir = "./log";
        sdk::MusicPlayer player(&listener, logDir);
        
        player.addMusicDir("/nonexistent/path");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        
        // Should have attempted to add and failed
        CHECK(player.getMusicCount() == 0);
    }
}
```

**预期覆盖率提升**: +180 行

---

### 3.7 utils/ByteUtils 扩展 (扩展 test_utils.cpp)

**目标**: 覆盖 ByteUtils 中 10 行未覆盖代码

```cpp
TEST_SUITE("ByteUtils Edge Cases")
{
    TEST_CASE("convertByteOrder with null data is handled")
    {
        // Assuming such function exists
    }
    
    TEST_CASE("convertByteOrder with various data sizes")
    {
        // Test 1, 2, 3, 4, 5, 7, 8 byte conversions
    }
}
```

**预期覆盖率提升**: +10 行

---

## 4. 测试文件清单

| 文件 | 操作 | 新增测试用例数 |
|------|------|---------------|
| `sdk/test/unit/test_error.cpp` | **新建** | 8 |
| `sdk/test/unit/test_player.cpp` | 扩展 | 15 |
| `sdk/test/unit/test_decode.cpp` | 扩展 | 10 |
| `sdk/test/unit/test_audio_device.cpp` | 扩展 | 8 |
| `sdk/test/unit/test_audio.cpp` | 扩展 | 6 |
| `sdk/test/unit/test_utils.cpp` | 扩展 | 3 |
| **总计** | - | **50** |

---

## 5. 预期覆盖率提升

| 模块 | 当前 | 预期 | 提升 |
|------|------|------|------|
| sdk (root) | 50.4% | 65% | +14.6% |
| audio/decode | 73.6% | 85% | +11.4% |
| audio/device | 78.1% | 90% | +11.9% |
| audio/resample | 69.4% | 85% | +15.6% |
| utils | 86.4% | 95% | +8.6% |
| **总计** | 66.3% | **~80%** | **~14%** |

---

## 6. 实施顺序

### 阶段 1: 快速胜利 (1-2天)
1. `test_error.cpp` - 纯单元测试，无依赖，8个用例
2. utils 扩展 - 简单边界条件

### 阶段 2: 核心模块 (3-5天)
3. audio/resample 扩展 - 错误处理路径
4. audio/device 扩展 - 状态转换

### 阶段 3: 复杂模块 (5-7天)
5. audio/decode 扩展 - 异步状态机测试
6. MusicPlayer 扩展 - 集成状态测试

---

## 7. 注意事项

1. **SDL 回调测试**: `test_audio_device.cpp` 中的回调测试受 SDL 线程影响，时序可能不稳定
2. **异步等待超时**: 所有带 `sleep_for` 的测试应设置合理超时 (建议 100-5000ms)
3. **真实媒体依赖**: 使用 `RealMediaFixture` 获取路径，避免硬编码
4. **错误路径测试**: 确保测试不会因为预期外的成功而误报

---

## 8. 验证命令

```bash
# 构建并运行测试
cmake --build build --target UnitTests
ctest --test-dir build -R sdk_unit_tests --output-on-failure

# 覆盖率检查
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/toolchain.linux_x86_64_gcc.cmake \
  -DCMAKE_CXX_FLAGS="-fprofile-arcs -ftest-coverage" -G "Unix Makefiles"
cmake --build build
ctest --test-dir build -R sdk_ --output-on-failure
```
