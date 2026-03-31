# UNIT TESTS KNOWLEDGE BASE

## OVERVIEW
`sdk/test/unit/` 是 doctest 单元测试子树：所有文件统一编进 `UnitTests` 可执行文件，由 `UnitTestsMain.cpp` 初始化日志后一次性运行。

## STRUCTURE
```text
sdk/test/unit/
├── UnitTestsMain.cpp         # doctest main + LogWrapper 初始化
├── test_audio*.cpp           # AudioResample / AudioDevice / AudioFormat 工具
├── test_decode.cpp           # AudioDecodeProcess / FLAC / Vorbis / AudioStreamDecoder
├── test_extractor.cpp        # ExtractorFactory 与真实媒体 extractor 行为
├── test_playlist.cpp         # MusicPlayList / FileProperties
├── test_utils.cpp            # AudioRingBuffer / ByteUtils / AudioBuffer
└── test_cosmos.cpp           # Optional / Lazy / ScopeGuard / Range / Timer
```

## WHERE TO LOOK
| 任务 | 位置 | 备注 |
|------|------|------|
| doctest 入口 | `UnitTestsMain.cpp` | `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` + `./log/unittest` |
| 音频设备/格式工具 | `test_audio_device.cpp`, `test_audio.cpp` | SDL 行为 + AudioFormat/Resample |
| 解码状态机 | `test_decode.cpp` | wait/poll 辅助函数、真实媒体 decode |
| 提取器单测 | `test_extractor.cpp` | factory、sniff、真实媒体样本 |
| 播放列表行为 | `test_playlist.cpp` | `WorkQueue` + `promise/future` 协调异步添加 |
| 基础工具/容器 | `test_utils.cpp`, `test_cosmos.cpp` | 纯单元级，无媒体依赖 |

## CONVENTIONS
- 新 doctest 用 `TEST_SUITE` + `TEST_CASE` 组织；相关断言优先 `REQUIRE`，只有允许继续收集更多失败信息时再用 `CHECK`。
- 这个子目录里的真实媒体依赖统一经 `RealMediaFixture` 获取；不要手写 `../../music/...` 路径。
- `UnitTestsMain.cpp` 已负责日志初始化；不要在单个用例里重复初始化 `LogWrapper`。
- 异步接口测试沿用现有模式：`std::promise/std::future` 等待回调完成，或用轮询辅助函数验证状态迁移，而不是裸 `sleep` 兜底；`test_audio_device.cpp` 是少数例外，因为它在等 SDL callback。

## ANTI-PATTERNS
- 不要新增 `unit/*.cpp` 却忘了同步 `sdk/test/CMakeLists.txt` 的 `UNIT_TEST_SOURCES`。
- 不要把需要旧框架分组参数或媒体冒烟入口的测试硬塞进 doctest；那类测试应留在 `TestSdkSuite.cpp`。
- 不要把 `test_audio_device.cpp` 的回调时序结论推广到所有模块；它受 SDL 线程与环境设备可用性影响。
- 不要在 `test_playlist.cpp` 里用永不完成的回调等待；这里的 `future.wait()` 会直接把测试卡死。

## LOCAL CHECKS
```bash
cmake --build build --target UnitTests
ctest --test-dir build -R sdk_unit_tests --output-on-failure
ctest --test-dir build -R sdk_player_tests --output-on-failure
ctest --test-dir build -R sdk_extractor_spec_tests --output-on-failure
```

## NOTES
- `test_decode.cpp` 和 `test_extractor.cpp` 会拉真实媒体样本，是这个子目录里最接近“集成边界”的两组 doctest；改解码/提取器时优先回看它们。
- `test_playlist.cpp` 用静态 `WorkQueue` 和回调同步封装测试夹具；改播放列表异步逻辑时先核这些 helper 是否还成立。
- `test_cosmos.cpp` / `test_utils.cpp` 更像纯内存单测，失败时通常不需要跑整套媒体回归。
