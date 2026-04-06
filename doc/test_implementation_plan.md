# SoundBridge SDK 测试覆盖率提升实施计划

**版本**: v1.1  
**更新日期**: 2026-04-06  
**目标**: 将 SDK 代码覆盖率从 66.3% 提升至 80%+

---

## 1. 覆盖率现状与目标

| 模块 | 当前行覆盖 | 分支覆盖 | 目标 | 缺口 | 优先级 |
|------|-----------|---------|------|------|--------|
| error_impl.cpp | **0%** | N/A | 80% | 15 行 | 🔴 紧急 |
| MusicPlayer.cpp | **41%** | **35%** | 70% | ~350 行 | 🔴 紧急 |
| player_impl.cpp | ~50% | ~40% | 75% | ~100 行 | 🔴 紧急 |
| MusicPlayList.cpp | **83%** | **87%** | 95% | ~50 行 | 🟡 中 |
| AudioStreamDecoder.cpp | **65%** | **54%** | 85% | ~230 行 | 🔴 紧急 |
| AudioDecodeProcess.cpp | **88%** | **74%** | 95% | ~30 行 | 🟡 中 |
| AudioDevice.cpp | ~55% | ~40% | 85% | ~120 行 | 🔴 紧急 |
| AudioResample.cpp | **69%** | **100%** | 90% | ~50 行 | 🟡 中 |
| ByteUtils | 部分 | 部分 | 95% | ~10 行 | 🟢 低 |

---

## 2. 测试文件变更清单

### 2.1 新建文件

| 文件 | 测试用例数 | 依赖 | 复杂度 | 预计工时 |
|------|----------|------|--------|---------|
| `sdk/test/unit/test_error.cpp` | 10 | 无 | ⭐ | 0.5h |
| `sdk/test/unit/test_player_integration.cpp` | 12 | MockPlayerCallbacks | ⭐⭐ | 2h |
| `sdk/test/unit/test_stream_decoder.cpp` | 8 | AudioRingBuffer, MockExtractor | ⭐⭐⭐ | 3h |
| `sdk/test/unit/test_playlist_advance.cpp` | 6 | MusicPlayList, RealMediaFixture | ⭐⭐ | 1.5h |
| `sdk/test/unit/test_audio_device_lifecycle.cpp` | 7 | AudioDevice | ⭐⭐ | 1.5h |
| `sdk/test/unit/test_resample_formats.cpp` | 5 | AudioResample | ⭐⭐ | 1h |

**总计新建**: 48 个测试用例，~9.5h

### 2.2 扩展现有文件

| 文件 | 新增用例数 | 预计工时 |
|------|----------|---------|
| `sdk/test/unit/test_player.cpp` | +8 | 1h |
| `sdk/test/unit/test_decode.cpp` | +6 | 1.5h |
| `sdk/test/unit/test_audio_device.cpp` | +5 | 1h |
| `sdk/test/unit/test_audio.cpp` | +4 | 0.5h |
| `sdk/test/unit/test_utils.cpp` | +3 | 0.5h |

**总计扩展**: 26 个测试用例，~4.5h

---

## 3. 详细实施计划

### 阶段 0: 准备 (0.5h)

- [ ] 确认 RealMediaFixture 可用媒体文件列表
- [ ] 确认 MockPlayerCallbacks, MockExtractor 等 Mock 完整性
- [ ] 创建必要的辅助函数（waitForDecoderState 等）

### 阶段 1: 新建测试文件 (并行开发)

#### 1.1 test_error.cpp ⭐⭐⭐

**优先级**: 🔴 最高  
**目标**: 覆盖 error_impl.cpp (0%) 和 ErrorCode.cpp (~60%)

```cpp
// 伪代码结构
#include <doctest/doctest.h>
#include <soundbridge/error.h>

TEST_SUITE("ErrorCode (soundbridge namespace)")
{
    // 测试 GetErrorInfo 对所有 ErrorCode 枚举值
    // 测试 ToString 对 ErrorModule/ErrorSeverity/ErrorAction
    // 测试 FormatError 带/不带 detail
    // 测试 unknown code 返回 Unknown info
}
```

**验收标准**: 
- 编译通过
- 覆盖 error_impl.cpp 所有 5 个函数
- 覆盖 ErrorCode.cpp 所有 switch 分支

#### 1.2 test_player_integration.cpp ⭐⭐⭐

**优先级**: 🔴 最高  
**目标**: 覆盖 MusicPlayer.cpp 回调路径 (~350 行)

```cpp
// 测试场景
TEST_CASE("getAudioData callback writes to ring buffer")
TEST_CASE("putMusicPlayListCurBuf updates current track")
TEST_CASE("updateMusicList triggers onMusicPlayerMusicListChanged")
TEST_CASE("onMusicPlayListError with autoSkipOnError")
TEST_CASE("onMusicPlayListError without autoSkipOnError")
TEST_CASE("updatePlayState triggers state callback")
TEST_CASE("fillSilence when ring buffer is empty")
```

**验收标准**: MusicPlayer.cpp 行覆盖率 > 65%

#### 1.3 test_stream_decoder.cpp ⭐⭐⭐

**优先级**: 🔴 最高  
**目标**: 覆盖 AudioStreamDecoder.cpp 全部函数

```cpp
// 测试场景
TEST_CASE("start with null ring returns INVALID_OPERATION")
TEST_CASE("start with null extractor returns INVALID_OPERATION")
TEST_CASE("start with valid extractor transitions to DECODING")
TEST_CASE("stop transitions from DECODING to IDLE")
TEST_CASE("seekToMs during decoding transitions to SEEKING then back")
TEST_CASE("threadFunc processes FLAC frames")
TEST_CASE("threadFunc processes Vorbis frames")
TEST_CASE("threadFunc processes generic (AAC/MP3) frames")
```

**验收标准**: AudioStreamDecoder.cpp 行覆盖率 > 80%

#### 1.4 test_playlist_advance.cpp ⭐⭐

**优先级**: 🟡 中  
**目标**: 覆盖 MusicPlayList::advanceToNextTrack 和 skipToNextPlayable

```cpp
// 测试场景
TEST_CASE("CurrentItemOnce advanceToNextTrack returns false")
TEST_CASE("CurrentItemInLoop advanceToNextTrack stays on same track")
TEST_CASE("Sequential advanceToNextTrack moves to next")
TEST_CASE("Sequential at end returns false")
TEST_CASE("Loop advanceToNextTrack wraps to first")
TEST_CASE("Random advanceToNextTrack picks random track")
```

**验收标准**: MusicPlayList.cpp 行覆盖率 > 93%

#### 1.5 test_audio_device_lifecycle.cpp ⭐⭐

**优先级**: 🟡 中  
**目标**: 覆盖 AudioDevice.cpp 状态转换

```cpp
// 测试场景
TEST_CASE("open -> start -> stop -> close lifecycle")
TEST_CASE("open twice returns error")
TEST_CASE("start without open returns error")
TEST_CASE("stop without start is safe")
TEST_CASE("close without open is safe")
TEST_CASE("write fills internal buffer")
TEST_CASE("clearQueue resets buffer")
```

**验收标准**: AudioDevice.cpp 行覆盖率 > 75%

#### 1.6 test_resample_formats.cpp ⭐⭐

**优先级**: 🟡 中  
**目标**: 覆盖 AudioResample 非 S16 格式

```cpp
// 测试场景
TEST_CASE("resample S16 to S16 (no conversion)")
TEST_CASE("resample S32 to S16")
TEST_CASE("resample FLT32 to S16")
TEST_CASE("resample DBL64 to S16")
TEST_CASE("resample with invalid input format returns error")
```

**验收标准**: AudioResample.cpp 行覆盖率 > 85%

### 阶段 2: 扩展现有测试 (串行或并行)

#### 2.1 扩展 test_player.cpp

- [ ] +3: Player pause/stop/state 测试
- [ ] +2: Player seek 测试
- [ ] +3: Player navigation 测试

#### 2.2 扩展 test_decode.cpp

- [ ] +2: FLACDecode 错误路径
- [ ] +2: VorbisDecode 错误路径  
- [ ] +2: AudioDecodeProcess init 失败

#### 2.3 扩展 test_audio_device.cpp

- [ ] +2: getDeviceList 测试
- [ ] +3: write 边界条件

#### 2.4 扩展 test_audio.cpp

- [ ] +2: AudioResample null 输入
- [ ] +2: AudioResample 零长度输入

#### 2.5 扩展 test_utils.cpp

- [ ] +3: ByteUtils 边界条件

### 阶段 3: 集成与验证 (2h)

- [ ] 添加新文件到 CMakeLists.txt
- [ ] 构建并修复编译错误
- [ ] 运行所有单元测试
- [ ] 运行覆盖率测试
- [ ] 分析覆盖率报告
- [ ] 迭代补测遗漏点

---

## 4. 任务分解与并行机会

```
阶段 1 (可并行执行):
├─ Agent A: test_error.cpp (0.5h)
├─ Agent B: test_player_integration.cpp (2h)
├─ Agent C: test_stream_decoder.cpp (3h)
├─ Agent D: test_playlist_advance.cpp (1.5h)
├─ Agent E: test_audio_device_lifecycle.cpp (1.5h)
└─ Agent F: test_resample_formats.cpp (1h)

阶段 2 (可并行执行):
├─ Agent G: test_player.cpp 扩展 (1h)
├─ Agent H: test_decode.cpp 扩展 (1.5h)
├─ Agent I: test_audio_device.cpp 扩展 (1h)
├─ Agent J: test_audio.cpp 扩展 (0.5h)
└─ Agent K: test_utils.cpp 扩展 (0.5h)

阶段 3 (串行):
└─ 集成验证 (2h)
```

**理论最短时间**: ~6h (阶段1并行) + ~1.5h (阶段2并行) + 2h (阶段3) = **~9.5h**

---

## 5. 依赖关系

```
test_error.cpp
  └─ 依赖: soundbridge/error.h, ErrorCode.hpp

test_player_integration.cpp
  ├─ 依赖: test_player.cpp (MockPlayerCallbacks)
  └─ 依赖: MusicPlayer.cpp 实现

test_stream_decoder.cpp
  ├─ 依赖: test_decode.cpp (MockExtractor, waitForDecoderState)
  └─ 依赖: AudioRingBuffer, AudioSpec

test_playlist_advance.cpp
  ├─ 依赖: RealMediaFixture
  └─ 依赖: MusicPlayList.cpp

test_audio_device_lifecycle.cpp
  └─ 依赖: AudioDevice.cpp

test_resample_formats.cpp
  └─ 依赖: AudioResample.cpp, AudioFormat 定义
```

---

## 6. 风险与缓解

| 风险 | 可能性 | 影响 | 缓解措施 |
|------|--------|------|---------|
| SDL 回调测试不稳定 | 中 | 中 | 使用 sleep_for(200ms) 轮询，设置合理超时 |
| 异步测试超时 | 中 | 高 | 使用 std::promise/future，设置 5s 超时 |
| Mock 不完整 | 低 | 高 | 先审查 MockPlayerCallbacks/MockExtractor 完整性 |
| 媒体文件缺失 | 低 | 中 | 使用 RealMediaFixture.openOrSkip() |
| 覆盖率数据过期 | 中 | 中 | 实施前重新运行测试生成 gcov |

---

## 7. 验收标准

### 7.1 功能验收
- [ ] 所有新增测试编译通过
- [ ] 所有现有测试仍然通过
- [ ] 无新增警告 (警告视为错误)

### 7.2 覆盖率验收

| 模块 | 起始 | 目标 | 必须达标 |
|------|------|------|---------|
| error_impl.cpp | 0% | 80%+ | ✅ |
| MusicPlayer.cpp | 41% | 65%+ | ✅ |
| AudioStreamDecoder.cpp | 65% | 80%+ | ✅ |
| AudioDevice.cpp | 55% | 75%+ | ✅ |
| **总计** | **66.3%** | **80%** | ✅ |

### 7.3 代码质量验收
- [ ] 遵循项目代码风格
- [ ] 测试用例命名清晰
- [ ] 包含必要的注释
- [ ] 无硬编码路径

---

## 8. 实施检查清单

### 预检
- [ ] 确认 git 分支已创建
- [ ] 确认构建环境可用
- [ ] 确认 RealMediaFixture 可用

### 阶段 1
- [ ] test_error.cpp 实现并通过
- [ ] test_player_integration.cpp 实现并通过
- [ ] test_stream_decoder.cpp 实现并通过
- [ ] test_playlist_advance.cpp 实现并通过
- [ ] test_audio_device_lifecycle.cpp 实现并通过
- [ ] test_resample_formats.cpp 实现并通过

### 阶段 2
- [ ] test_player.cpp 扩展
- [ ] test_decode.cpp 扩展
- [ ] test_audio_device.cpp 扩展
- [ ] test_audio.cpp 扩展
- [ ] test_utils.cpp 扩展

### 阶段 3
- [ ] CMakeLists.txt 更新
- [ ] 完整构建通过
- [ ] 所有测试通过
- [ ] 覆盖率达标
- [ ] PR 创建

---

## 9. 执行命令

```bash
# 1. 创建分支
git checkout -b feature/sdk-test-coverage

# 2. 配置覆盖率构建
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/toolchain.linux_x86_64_gcc.cmake \
  -DCMAKE_CXX_FLAGS="-fprofile-arcs -ftest-coverage" -G "Unix Makefiles"

# 3. 构建测试
cmake --build build --target UnitTests -j$(nproc)

# 4. 运行测试
ctest --test-dir build -R sdk_unit_tests --output-on-failure

# 5. 运行覆盖率测试
ctest --test-dir build -R sdk_ --output-on-failure

# 6. 检查覆盖率
# (使用 gcov 或 lcov 生成报告)
```

---

## 10. 里程碑

| 里程碑 | 完成标准 | 预计时间 |
|--------|---------|---------|
| M1: 基础设施 | test_error.cpp 通过，覆盖 0%→80% | Day 1 |
| M2: 核心模块 | test_stream_decoder + test_player_integration 通过 | Day 1-2 |
| M3: 播放列表 | test_playlist_advance 通过 | Day 2 |
| M4: 设备/重采样 | test_audio_device_lifecycle + test_resample_formats 通过 | Day 2-3 |
| M5: 扩展完成 | 所有扩展测试通过 | Day 3 |
| M6: 集成验证 | 覆盖率达标，PR 就绪 | Day 3-4 |
