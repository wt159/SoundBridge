# SoundBridge SDK 测试框架设计文档

## 1. 概述

### 1.1 测试框架现状

| 框架 | 用途 | 状态 |
|------|------|------|
| **Doctest** | 单元测试主力 | ✅ 统一框架 |
| **TestSdkSuite** | 遗留自定义测试 | ⚠️ 已废弃 (保留向后兼容) |

### 1.2 测试目标

1. **统一测试框架**：所有测试使用 doctest
2. **提升测试覆盖率**：当前 66.18%，目标 80%+
3. **支持代码覆盖率**：集成 gcov/lcov
4. **简化维护**：单一断言风格，单一入口

---

## 2. 测试架构

### 2.1 整体架构

```
                        ┌──────────────────────┐
                        │   CMake + CTest      │
                        └──────────────────────┘
                                       │
                    ┌───────────────────┼───────────────────┐
                    ▼                   ▼                   ▼
             ┌───────────┐       ┌───────────┐       ┌───────────┐
             │  Build    │       │   Test    │       │ Coverage │
             │ (cmake)   │──────▶│ (ctest)   │──────▶│ gcov/lcov │
             └───────────┘       └───────────┘       └───────────┘
                                          │
                          ┌────────────────┴────────────────┐
                          ▼                                 ▼
                   ┌───────────────┐               ┌─────────────────┐
                   │ UnitTests     │               │ TestSdkSuite    │
                   │ doctest       │               │ deprecated      │
                   │ 统一测试入口   │               │ 向后兼容        │
                   └───────────────┘               └─────────────────┘
                          │
              ┌───────────┼───────────┬─────────────┐
              ▼           ▼           ▼             ▼
          Cosmos      Utils      Audio          Player
```

### 2.2 目录结构

```
sdk/test/
├── CMakeLists.txt                 # 测试入口配置 (统一)
├── TestSdkSuite.cpp              # 遗留入口 (已废弃,保留兼容)
├── UnitTestsMain.cpp             # doctest main + LogWrapper 初始化
├── unit/
│   ├── test_cosmos.cpp              # Cosmos 组件测试
│   ├── test_utils.cpp                # Utils 组件测试
│   ├── test_audio.cpp                # AudioResample 模块测试
│   ├── test_audio_device.cpp         # AudioDevice 模块测试
│   ├── test_audio_common.cpp         # AudioCommon 迁移测试
│   ├── test_extractor.cpp            # Extractor 模块测试
│   ├── test_extractor_isolated.cpp   # Extractor 隔离测试
│   ├── test_decode.cpp               # AudioDecode 模块测试
│   ├── test_playlist.cpp             # MusicPlayList 测试
│   ├── test_player.cpp               # Player API 测试
│   └── test_media_smoke.cpp          # 媒体冒烟测试
├── fixtures/
│   ├── RealMediaFixture.hpp          # 真实媒体测试固件
│   └── TempDirFixture.hpp            # 临时目录固件
├── mocks/
│   ├── AudioMocks.hpp                # 音频回调 Mock
│   └── PlayerMocks.hpp               # Player API Mock (新增)
└── thirdparty/
    └── doctest/doctest.h             # Doctest header
```

---

## 3. 运行测试

### 3.1 构建测试

```bash
# 配置
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/toolchain.linux_x86_64_gcc.cmake

# 构建 UnitTests
cmake --build build --target UnitTests
```

### 3.2 运行测试

```bash
# 运行所有 SDK 测试
ctest --test-dir build -R sdk_ --output-on-failure

# 运行单元测试 (主入口)
ctest --test-dir build -R sdk_unit_tests --output-on-failure

# 运行覆盖率报告
cmake --build build --target coverage
```

---

## 4. 模块覆盖率

### 4.1 覆盖率目标

| 模块 | 目标覆盖率 | 状态 |
|------|-----------|------|
| cosmos | 95% | ✅ 已有 91.89% |
| log | 99% | ✅ 已有 98.85% |
| utils | 90% | 已有 83.82% |
| audio/resample | 80% | 已有 69.41% |
| audio/decode | 75% | 已有 64.42% |
| audio/device | 75% | 已有 69.71% |
| extractor | 70% | 约 50% |
| **整体 SDK** | **80%** | 已有 66.18% |

### 4.2 已知限制

- **gcov 多线程限制**：SDL 等第三方库未使用 coverage 标志编译，AudioDevice 的 `audioCallback` 不会被 gcov 记录

---

## 5. 测试用例统计

| 测试套件 | 用例数 | 说明 |
|----------|--------|------|
| Cosmos::Optional | 12 | 空状态、构造、拷贝、移动、赋值、比较 |
| Cosmos::Lazy | 2 | 延迟初始化、引用返回 |
| Cosmos::ScopeGuard | 2 | 创建、Dismiss |
| Cosmos::Range | 6 | 步长、大小、索引访问 |
| Cosmos::Timer | 7 | 构造、elapsed、timestamp、Date |
| Cosmos::type_name | 5 | 类型名模板 |
| AudioRingBuffer | 9 | 读写、环绕、reset、容量 |
| ByteUtils | 8 | 字节序、U16/U32/U64、FourCC |
| AudioResample | 6 | 构造、init、resample |
| AudioCommon | 4 | 格式映射、字节计算 |
| AudioDevice | 22 | open/close、start/stop、callback |
| DataSource/Extractor | ~50 | 接口测试、工厂模式、真实媒体 |
| AudioDecode | ~30 | 管线、解码、FLAC、Vorbis |
| MusicPlayList | ~27 | 播放列表操作 |
| Player API | ~15 | MusicPlayer、Public Player |
| MediaSmoke | ~12 | 媒体格式冒烟测试 |
| **总计** | **~250** | 统一框架 |

---

## 6. 附录

### 6.1 Doctest 常用语法

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

// 基本断言
REQUIRE(expression);           // 失败终止
CHECK(expression);            // 失败继续

// 异常断言
REQUIRE_THROWS(expr);
REQUIRE_THROWS_AS(expr, type);

// 比较断言
REQUIRE_EQ(a, b);
REQUIRE_NE(a, b);
REQUIRE_GT(a, b);
```

### 6.2 Mock 库使用

```cpp
#include "mocks/PlayerMocks.hpp"
#include "mocks/AudioMocks.hpp"

// 使用 MockPlayerCallbacks
MockPlayerCallbacks playerCallback;
soundbridge::Player player(&playerCallback, config);

// 使用 MockAudioDecodeCallback
MockAudioDecodeCallback decodeCallback;
AudioDecode decode(AUDIO_CODEC_ID_AAC, &decodeCallback);
```

### 6.3 参考资料

- [Doctest 文档](https://github.com/onqtam/doctest)
- [lcov 覆盖率工具](https://github.com/linux-test-project/lcov)

---

## 7. 迁移进度

### 7.1 已完成

- [x] Phase 1: CMake 配置统一
- [x] Phase 2: Legacy 测试迁移到 doctest
- [x] PlayerMocks.hpp 创建
- [x] test_player.cpp (Player API 测试)
- [x] test_media_smoke.cpp (媒体冒烟测试)
- [x] test_audio_common.cpp (AudioCommon 测试)
- [x] test_extractor_isolated.cpp (Extractor 隔离测试)

### 7.2 下一步

1. **覆盖率提升**：通过 Phase 4 持续改进
2. **边界测试**：完善 Extractor 边界情况
3. **TestSdkSuite 移除**：确认无依赖后删除 legacy 入口
