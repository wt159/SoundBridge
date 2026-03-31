# SoundBridge SDK 测试框架设计文档

## 1. 概述

### 1.1 测试框架现状

| 框架 | 用途 | 状态 |
|------|------|------|
| **Doctest** | 单元测试主力 | ✅ 主力框架 |
| **TestSdkSuite** | 遗留自定义测试 | ✅ 兼容层 |

### 1.2 测试目标

1. **提升测试覆盖率**：当前 66.18%，目标 80%+
2. **支持代码覆盖率**：集成 gcov/lcov
3. **渐进式迁移**：保留现有测试，逐步扩展

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
                       ┌────────────────────┴────────────────────┐
                       ▼                                     ▼
               ┌───────────────┐                     ┌────────────────┐
               │ UnitTests     │                     │ TestSdkSuite   │
               │ doctest       │                     │ legacy custom  │
               │ 单元测试      │                     │ 媒体冒烟测试   │
               └───────────────┘                     └────────────────┘
                       │                                     │
           ┌───────────┼───────────┐               ┌───────┴────────┐
           ▼           ▼           ▼               ▼                ▼
        Cosmos      Utils      Audio           Extractor         Player
```

### 2.2 目录结构

```
sdk/test/
├── CMakeLists.txt                 # 测试入口配置
├── TestSdkSuite.cpp              # 遗留自定义测试入口
├── UnitTestsMain.cpp             # doctest main + LogWrapper 初始化
├── unit/
│   ├── test_cosmos.cpp         # Cosmos 组件测试
│   ├── test_utils.cpp           # Utils 组件测试
│   ├── test_audio.cpp          # AudioResample 模块测试
│   ├── test_audio_device.cpp    # AudioDevice 模块测试
│   ├── test_extractor.cpp       # Extractor 模块测试
│   ├── test_decode.cpp          # AudioDecode 模块测试
│   └── test_playlist.cpp        # MusicPlayList 测试
├── fixtures/
│   ├── RealMediaFixture.hpp     # 真实媒体测试固件
│   └── TempDirFixture.hpp       # 临时目录固件
├── mocks/
│   └── AudioMocks.hpp           # 音频回调 Mock
└── thirdparty/
    └── doctest/doctest.h        # Doctest header
```

---

## 3. 运行测试

### 3.1 构建测试

```bash
mkdir build && cd build
cmake .. --no-warn-unused-cli -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain/toolchain.linux_x86_64_gcc.cmake \
  -G "Unix Makefiles"
cmake --build . --target UnitTests
```

### 3.2 运行测试

```bash
# 运行所有 SDK 测试
ctest -R sdk_ --output-on-failure

# 运行单元测试
ctest -R sdk_unit_tests --output-on-failure

# 运行覆盖率报告
ctest -T Coverage --output-on-failure
```

---

## 4. 模块覆盖率

### 4.1 当前覆盖率 (2026-03-31)

| 模块 | 覆盖率 | 说明 |
|------|--------|------|
| audio/common | 68.00% | AudioCommon.hpp |
| audio/decode | 64.42% | AudioDecode, AudioStreamDecoder, FLACDecode, VorbisDecode |
| audio/device | 69.71% | AudioDevice (gcov 多线程限制) |
| audio/resample | 69.41% | AudioResample |
| **cosmos** | **91.89%** | ✅ 已达标 |
| extractor | ~50% | 各格式提取器 |
| **log** | **98.85%** | ✅ 已达标 |
| utils | 83.82% | AudioBuffer, AudioRingBuffer, ByteUtils |
| **整体 SDK** | **66.18%** | 目标 80% |

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
| AudioFormat utilities | 2 | 格式转换辅助函数 |
| AudioSpec | 2 | 默认构造、相等比较 |
| AudioBuffer | 4 | 构造、setData、getData |
| AudioCodecConfig | 2 | 默认构造、属性设置 |
| AudioDecode | 5 | 构造、解码、null 处理 |
| AudioDecodeProcess | 2 | 默认值 |
| FLACDecode | 4 | 构造、解码、buffer 设置、abort |
| VorbisDecode | 3 | 构造、解码、abort |
| AudioDevice | 22 | open/close、start/stop、callback |
| DataSource/Extractor | ~30 | 接口测试、工厂模式、真实媒体 |
| MusicPlayList | ~20 | 播放列表操作 |
| **总计** | **~150** | |

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

### 6.2 参考资料

- [Doctest 文档](https://github.com/onqtam/doctest)
- [lcov 覆盖率工具](https://github.com/linux-test-project/lcov)

---

## 7. 下一步计划

### 7.1 覆盖率提升

| 模块 | 当前 | 目标 | 差距 |
|------|------|------|------|
| extractor | ~50% | 70% | ~20% |
| audio/decode | 64.42% | 75% | ~10% |
| 整体 | 66.18% | 80% | ~14% |

### 7.2 建议工作

1. **Extractor 扩展**：更多真实媒体格式测试
2. **AudioDecode 深度**：完整解码流程、seek 功能
3. **MusicPlayer 集成**：播放控制、状态机
