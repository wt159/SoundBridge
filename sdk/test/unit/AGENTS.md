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
├── test_media_coverage.cpp   # 全格式解码覆盖度与时长基线（判据见 doc/format-support.md）
├── test_playlist.cpp         # MusicPlayList / FileProperties
├── test_http_source.cpp      # HttpSource / URL 分派 / HLS VOD 直通（本地 mock server，零公网依赖）
├── test_live_source.cpp      # LIVE 重连状态机事件计数判据（LiveSourceReconnect 套件）
├── test_utils.cpp            # AudioRingBuffer / ByteUtils / AudioBuffer
└── test_cosmos.cpp           # Optional / Lazy / ScopeGuard / Range / Timer / WorkQueue
```

`../LiveSmoke.cpp`（`sdk/test/` 根）是独立可执行（D23）：注册构建、不注册 ctest，承接 LIVE / ICY 稳态 8s 与 `stop()` 有限时间返回的手工冒烟；不在本子目录树内，也不编进 `UnitTests`。

上面的树只列**主要入口，不是穷举**（该目录现有 26 个 `test_*.cpp`）。找具体套件用 `grep -rn 'TEST_SUITE(' sdk/test/unit`，不要据树的缺失判断某个套件不存在。

## WHERE TO LOOK
| 任务 | 位置 | 备注 |
|------|------|------|
| doctest 入口 | `UnitTestsMain.cpp` | `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` + `./log/unittest` |
| 音频设备/格式工具 | `test_audio_device.cpp`, `test_audio.cpp` | SDL 行为 + AudioFormat/Resample |
| 解码状态机 | `test_decode.cpp` | wait/poll 辅助函数、真实媒体 decode |
| 提取器单测 | `test_extractor.cpp` | factory、sniff、真实媒体样本 |
| 解码覆盖度基线 | `test_media_coverage.cpp` | 双向容差判据、必备素材清单；理由见 `doc/format-support.md` |
| 播放列表行为 | `test_playlist.cpp` | `WorkQueue` + `promise/future` 协调异步添加 |
| 网络源 / URL 分派 / HLS | `test_http_source.cpp` | 本地 mock HTTP/HLS server，零公网依赖；契约见 `doc/network-audio-source-spec.md` |
| LIVE 重连 / 停发形态判据 | `test_live_source.cpp` | `LiveSourceReconnect` 套件；mock 注入见 `../mocks/HttpMocks.hpp`；契约见 `doc/live-source-spec.md` §5.3 / §10 |
| LIVE / ICY 手工冒烟 | `../LiveSmoke.cpp` | 独立可执行、不注册 ctest；稳态 8s + `stop()` 上界，D23 判据载体 |
| 基础工具/容器 | `test_utils.cpp`, `test_cosmos.cpp` | 纯单元级，无媒体依赖 |
| 软件增益数学 | `test_audio_common.cpp` | `PcmGain` 套件：按格式缩放的半幅/零增益/U8 中点/渐变端点 + 未识别格式直通（判据 `UI-VOL2`，ADR 0013） |
| 音量契约（钳制与静音） | `test_player.cpp` | `MusicPlayerVolume` 套件：默认 100 与越界钳制、静音不改取值（判据 `UI-VOL1`） |
| 曲目身份键派生 | `test_playlist.cpp` | `MusicId` 套件：空路径 / URL 原样 / 点段折叠（含越根）/ 重复分隔符 / 相对路径绝对化 / 混合分隔符与大小写 / 盘符与 UNC（ADR 0014） |

## CONVENTIONS
- 新 doctest 用 `TEST_SUITE` + `TEST_CASE` 组织；相关断言优先 `REQUIRE`，只有允许继续收集更多失败信息时再用 `CHECK`。
- 这个子目录里的真实媒体依赖统一经 `RealMediaFixture` 获取；不要手写 `../../music/...` 路径。
- `UnitTestsMain.cpp` 已负责日志初始化；不要在单个用例里重复初始化 `LogWrapper`。
- 异步接口测试沿用现有模式：`std::promise/std::future` 等待回调完成，或用轮询辅助函数验证状态迁移，而不是裸 `sleep` 兜底；`test_audio_device.cpp` 是少数例外，因为它在等 SDL callback。
- 时长/覆盖度断言用 `RealMediaFixture::coverageToleranceMs()`，不要自己写一个百分比；实测侧必须取解码后的 spec，取提取器 spec 会在 fltp 格式上算出 2 倍（见 `doc/format-support.md`）。

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
- `test_cosmos.cpp` 的 `Cosmos::WorkQueue` 套件（#142）盯三条结构不变量：任务 id 单调非零 / 队列自身是非空真源 / `stop()` 后队列必清空；改 `WorkQueue.hpp` 必跑它。
- `test_live_source.cpp`（票 #159 / #162）按「事件计数为主、墙钟只作防挂死守卫」组织：注入面按 playlist 取回序号 / segment 请求序号锚定，不要改回全局请求序号（扫描期 `find_stream_info` 的 reload 次数随素材浮动）。FFmpeg hls 会跳过失败 segment 并继续推进窗口，单段失败不上提 `nullptr`；判据须让 playlist 侧触发重连或让首次 open 直接失败。
