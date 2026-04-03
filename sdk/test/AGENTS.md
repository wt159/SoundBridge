# SDK TEST KNOWLEDGE BASE

## OVERVIEW
`sdk/test/` 同时承载两套测试入口：遗留 `TestSdkSuite` 回归/媒体冒烟，以及 doctest `UnitTests` 单元测试。

## STRUCTURE
```text
sdk/test/
├── CMakeLists.txt            # 所有 CTest 名称都在这里注册
├── TestSdkSuite.cpp          # 自定义 check(...) 框架
├── unit/*.cpp                # doctest 主力测试；看 unit/AGENTS.md
├── fixtures/                 # 真实媒体 / 临时目录夹具
│   ├── RealMediaFixture.hpp  # 真实媒体路径获取（推荐使用）
│   └── TempDirFixture.hpp    # 临时目录固件
├── mocks/                    # 音频 mock
└── thirdparty/doctest/       # 本地 doctest 头文件；不要手改
```

## AGENTS HIERARCHY
- `unit/AGENTS.md`：doctest 入口、模块套件分布、异步/时序测试习惯。

## WHERE TO LOOK
| 任务 | 位置 | 备注 |
|------|------|------|
| 增删测试分组 | `CMakeLists.txt` | `sdk_core_tests` 等名字都在这里 |
| 回归/冒烟入口 | `TestSdkSuite.cpp` | 输出 `[PASS]/[FAIL]`，支持 `core\|decode\|media...` |
| Extractor 单测 | `unit/test_extractor.cpp` | DataSource / FileSource / 真实媒体路径 |
| Audio/Device 单测 | `unit/test_audio*.cpp`, `unit/test_decode.cpp` | doctest 风格 |
| Playlist 单测 | `unit/test_playlist.cpp` | 播放列表行为 |

## REAL MEDIA FIXTURE
**所有测试必须使用 `RealMediaFixture` 获取媒体路径，禁止硬编码路径**

```cpp
#include "fixtures/RealMediaFixture.hpp"

// 正确：使用 fixture
RealMediaFixture fixture;
std::string mediaPath = fixture.mediaPath("music.wav");
std::string mediaDir = fixture.mediaDir();

// 检查文件存在
if (fixture.exists("music.wav")) {
    // 使用媒体文件
}
```

## CONVENTIONS
- 旧框架断言只能用 `check(condition, message)`；新增到 `TestSdkSuite.cpp` 的测试要保持这种输出格式。
- 新单元测试优先放 `unit/*.cpp` 并使用 doctest；只有需要复用旧入口参数、真实媒体冒烟或分组兼容时再放 `TestSdkSuite.cpp`。
- 真实媒体路径由 `SB_MEDIA_DIR` / `SB_MEDIA_FILTER` / `SB_MEDIA_LIMIT` 控制；不要把机器本地绝对路径写死在测试里。
- Windows 运行测试依赖 `SOUNDBRIDGE_QT_TEST_BIN_DIR`；改测试启动方式时别破坏这条注入链。

## CODE COVERAGE (2026-04)
当前整体 SDK 覆盖率：**33.17%**

| Category | Coverage |
|----------|----------|
| Audio Decode | 40.63% |
| Audio Device | 43.56% |
| Audio Resample | 44.70% |
| Extractor Core | 46.05% |
| Extractors | 27.25% |
| Log | 55.63% |
| SDK Core | 31.98% |
| Utils | 45.95% |

生成覆盖率报告：
```bash
# 重新配置并编译（带覆盖率标志）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/toolchain.linux_x86_64_gcc.cmake \
  -DCMAKE_CXX_FLAGS="-fprofile-arcs -ftest-coverage" -G "Unix Makefiles"

# 构建并运行测试
cmake --build build
ctest --test-dir build -R sdk_ --output-on-failure

# gcov 文件在 build 目录各模块的 CMakeFiles/*.dir/ 目录下
```

## ANTI-PATTERNS
- 不要手改 `thirdparty/doctest/doctest.h`。
- 不要把覆盖率结果当成 `audio/device` 的唯一真相；SDL 回调线程有 gcov 盲区。
- 不要只新增测试文件、不补 `CMakeLists.txt`；那样 CTest 根本不会执行。
- 禁止硬编码媒体路径（如 `"../../music"`），必须使用 `RealMediaFixture`。

## LOCAL CHECKS
```bash
cmake --build build --target TestSdkSuite
cmake --build build --target UnitTests
ctest --test-dir build -R sdk_unit_tests --output-on-failure
ctest --test-dir build -R sdk_player_tests --output-on-failure
ctest --test-dir build -R sdk_extractor_spec_tests --output-on-failure
ctest --test-dir build -R sdk_media_smoke --output-on-failure
```

## NOTES
- `doc/testing_arch_design.md` 是测试设计背景；当前命令与注册名仍以本目录 `CMakeLists.txt` 为准。
- `sdk/test/log/` 是测试运行产物，不是源码目录。
