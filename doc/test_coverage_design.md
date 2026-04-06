# SoundBridge SDK 测试覆盖率提升方案

**文档版本**: v2.0  
**更新日期**: 2026-04-06  
**目标**: 将 SDK 代码覆盖率从 68% 提升至 80%+

---

## 1. 覆盖率统计方法

### 统计工具
使用 **gcov** (GCC 内置覆盖率工具) 分析 .gcda 文件

### 统计规则
```
gcov 输出格式:
  180:    8:  ...代码行...      ← 已覆盖 (执行 180 次)
  #####:    9:  ...代码行...   ← 未覆盖
        -:    0:Source:...     ← 非可执行行 (注释、空行)

可执行行 = 以数字开头 或 ##### 开头的行
已覆盖行 = 以数字开头（非 #####）
覆盖率 = 已覆盖行 / 可执行行 × 100%
```

### 统计范围
- 只统计 .cpp 源文件（不含 .hpp 头文件）
- 只统计 SDK 业务代码（排除 3rdparty）

---

## 2. 当前覆盖率分析 (v2.0)

| 模块 | 可执行行 | 已覆盖行 | 覆盖率 | 优先级 |
|------|---------|---------|--------|--------|
| sdk/ErrorCode.cpp | 92 | 92 | **100%** | ✅ |
| sdk/MusicPlayList.cpp | 218 | 181 | 83.0% | 🟡 中 |
| sdk/audio/resample/ | 168 | 144 | **85.7%** | 🟢 低 |
| sdk/player_impl.cpp | 173 | 123 | 71.1% | 🔴 高 |
| sdk/audio/device/ | 187 | 133 | 71.1% | 🔴 高 |
| sdk/audio/decode/ | 1012 | 764 | 75.5% | 🟡 中 |
| sdk/MusicPlayer.cpp | 384 | 248 | 64.6% | 🔴 高 |
| sdk/error_impl.cpp | 15 | 0 | **0.0%** | ⚠️ 缺失 |
| sdk/log/ | 85 | 84 | 98.8% | 🟢 低 |
| sdk/utils/ | 170 | 155 | 91.2% | 🟢 低 |
| **总计** | **2616** | **1974** | **75.5%** | - |

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

### 优先级 1: error_impl.cpp (0% → 目标 80%+)
- 新建 `test_error_bridge.cpp`
- 覆盖 soundbridge 命名空间所有错误包装函数
- 预计: +15 行覆盖

### 优先级 2: MusicPlayer.cpp (64.6% → 目标 75%+)
- 补充状态转换回调路径测试
- 新建 `test_player_callbacks.cpp`
- 预计: +50 行覆盖

### 优先级 3: AudioStreamDecoder.cpp (70.0% → 目标 80%+)
- 补充 FLAC/Vorbis 解码器边界条件
- 预计: +30 行覆盖

---

## 6. 验证命令

```bash
# 配置覆盖率构建
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/toolchain.linux_x86_64_gcc.cmake \
  -DCMAKE_CXX_FLAGS="-fprofile-arcs -ftest-coverage" -G "Unix Makefiles"

# 构建
cmake --build build

# 运行测试生成覆盖率数据
ctest --test-dir build -R sdk_unit_tests --output-on-failure

# 统计覆盖率 (各模块 CMakeFiles/*.dir/ 下)
for module in sdk audio/decode audio/device audio/resample; do
  find build -path "*$module*" -name "CMakeFiles" -type d | head -1 | xargs -I{} \
    gcov -o {} ../../../*.cpp 2>/dev/null
done
```

---

## 7. 历史版本

### v1.0 (2026-04-06)
- 初始覆盖率分析
- 计划目标: 80%+
