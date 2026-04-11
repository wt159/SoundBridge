# SoundBridge SDK 覆盖率计算与 CI 门禁方案

**文档版本**: v3.0  
**更新日期**: 2026-04-08  
**目标**: 建立 Linux-only 覆盖率采集、报告与 35% CI 门禁

---

## 1. 覆盖率统计方法

### 统计工具
使用 **lcov + genhtml** 生成覆盖率报告；底层仍基于编译器产出的 .gcda 数据。

### 统计规则
```
覆盖率数据格式:
  180:    8:  ...代码行...      ← 已覆盖 (执行 180 次)
  #####:    9:  ...代码行...   ← 未覆盖
        -:    0:Source:...     ← 非可执行行 (注释、空行)

可执行行 = 以数字开头 或 ##### 开头的行
已覆盖行 = 以数字开头（非 #####）
覆盖率 = 已覆盖行 / 可执行行 × 100%
```

### 统计范围
- 只统计 `sdk/` 下的编译单元源码（`.cpp/.cc/.cxx`）
- 排除 `sdk/test` 和 `sdk/3rdparty`
- 不统计头文件

---

## 2. v2.0 覆盖率快照

| 模块 | 当前覆盖率 | 备注 |
|------|-----------|------|
| Audio Decode | 40.63% | 当前覆盖率 |
| Audio Device | 43.56% | 当前覆盖率 |
| Audio Resample | 44.70% | 当前覆盖率 |
| Extractor Core | 46.05% | 当前覆盖率 |
| Extractors | 27.25% | 当前覆盖率 |
| Log | 55.63% | 当前覆盖率 |
| SDK Core | 31.98% | 当前覆盖率 |
| Utils | 45.95% | 当前覆盖率 |
| **整体 SDK** | **33.17%** | Linux 门禁 35% |

### 覆盖率提升记录

| 模块 | v1.0 (计划前) | v2.0 (实施后) | 提升 |
|------|---------------|---------------|------|
| sdk (root) | 50.4% | 69.8% | +19.4% |
| audio/decode | 73.6% | 75.5% | +1.9% |
| audio/device | 78.1% | 71.1% | -7.0% ⚠️ |
| audio/resample | 69.4% | 85.7% | +16.3% |
| log | 98.8% | 98.8% | — |
| utils | 86.4% | 91.2% | +4.8% |
| **总计** | **66.3%** | **75.5%** | **+9.2%** |

### ⚠️ 已知问题

1. **error_impl.cpp (0%)**: soundbridge 命名空间错误包装函数完全未覆盖
2. **MusicPlayer.cpp (64.6%)**: 136 行未覆盖，主要是状态转换回调路径
3. **AudioDevice.cpp**: SDL 设备枚举代码依赖实际硬件环境

---

## 3. 测试文件清单

### 已实施 (v2.0)

| 文件 | 操作 | 测试用例数 | 状态 |
|------|------|-----------|------|
| `test_error.cpp` | 新建 | 34 | ✅ |
| `test_player_integration.cpp` | 新建 | 16 | ✅ |
| `test_stream_decoder.cpp` | 新建 | 40 | ✅ |
| `test_playlist_advance.cpp` | 新建 | 19 | ✅ |
| `test_audio_device_lifecycle.cpp` | 新建 | 12 | ✅ |
| `test_resample_formats.cpp` | 新建 | 8 | ✅ |
| `test_audio.cpp` | 扩展 | ~6 | ✅ |
| `test_player.cpp` | 扩展 | ~8 | ✅ |
| **总计** | - | **~143** | ✅ |

### 待实施

| 文件 | 目标 | 覆盖文件 |
|------|------|---------|
| `test_error_bridge.cpp` | 覆盖 error_impl.cpp | soundbridge 命名空间包装函数 |
| `test_player_callbacks.cpp` | 覆盖 MusicPlayer.cpp 回调路径 | getAudioData, putMusicPlayListCurBuf 等 |

---

## 4. 新增测试用例设计

### 4.1 error_impl.cpp 测试 (待实施)

**目标**: 覆盖 soundbridge 命名空间错误包装函数

```cpp
TEST_SUITE("soundbridge Error Bridge")
{
    TEST_CASE("GetErrorInfo wraps sdk::GetErrorInfo")
    {
        auto info = soundbridge::GetErrorInfo(soundbridge::ErrorCode::Ok);
        CHECK(info.severity == soundbridge::ErrorSeverity::Info);
        CHECK(info.recoverable == true);
    }
    
    TEST_CASE("GetErrorInfo for all ErrorCode values")
    {
        // 测试所有枚举值的包装
        CHECK(soundbridge::GetErrorInfo(soundbridge::ErrorCode::FileOpenFailed).module 
              == soundbridge::ErrorModule::File);
        // ... 其他枚举值
    }
    
    TEST_CASE("ToString for ErrorModule")
    {
        CHECK_STREQ(soundbridge::ToString(soundbridge::ErrorModule::File), "File");
        CHECK_STREQ(soundbridge::ToString(soundbridge::ErrorModule::Player), "Player");
    }
    
    TEST_CASE("ToString for ErrorSeverity")
    {
        CHECK_STREQ(soundbridge::ToString(soundbridge::ErrorSeverity::Error), "Error");
    }
    
    TEST_CASE("ToString for ErrorAction")
    {
        CHECK_STREQ(soundbridge::ToString(soundbridge::ErrorAction::SkipTrack), "SkipTrack");
    }
    
    TEST_CASE("FormatError with detail")
    {
        auto result = soundbridge::FormatError(soundbridge::ErrorCode::DecodeFailed, "codec");
        CHECK(result.find("Decode failed") != std::string::npos);
    }
}
```

---

## 5. 后续提升计划

### 优先级 1: error_impl.cpp
- 新建 `test_error_bridge.cpp`
- 覆盖 soundbridge 命名空间所有错误包装函数

### 优先级 2: MusicPlayer.cpp
- 补充状态转换回调路径测试
- 新建 `test_player_callbacks.cpp`

### 优先级 3: AudioStreamDecoder.cpp
- 补充 FLAC/Vorbis 解码器边界条件

---

## 6. 验证命令

```bash
# 配置覆盖率构建（Linux only）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/toolchain.linux_x86_64_gcc.cmake \
  -DENABLE_COVERAGE=ON -G "Unix Makefiles"

# 构建与运行覆盖率目标
cmake --build build --target coverage

# 产物：build/coverage/html/index.html
# 产物：build/coverage/summary.txt
```

---

## 7. 历史版本

### v1.0 (2026-04-06)
- 初始覆盖率分析
- 计划目标: 统一测试框架 + 覆盖率门禁

### v3.0 (2026-04-08)
- 切换为 Linux-only `coverage` 目标
- 通过 `lcov/genhtml` 生成报告
- CI 门禁改为 35%
