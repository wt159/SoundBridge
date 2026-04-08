# AUDIO KNOWLEDGE BASE

## OVERVIEW
`sdk/audio/` 是播放链路的中段：`decode/` 负责解码与流式状态机，`resample/` 负责格式转换，`device/` 负责 SDL 输出，`common/` 放共享音频类型。

## STRUCTURE
```text
sdk/audio/
├── common/                # AudioSpec / AudioFormat 等共享定义
├── decode/                # AudioDecode / AudioDecodeProcess / AudioStreamDecoder；看 decode/AGENTS.md
├── resample/              # AudioResample + swresample 封装
└── device/                # AudioDevice + SDL callback 输出
```

## AGENTS HIERARCHY
- `decode/AGENTS.md`：解码器封装、流式状态机、解码相关回归。

## WHERE TO LOOK
| 任务 | 位置 | 备注 |
|------|------|------|
| 音频模块装配 | `CMakeLists.txt` | 只负责串起 `device/resample/decode` 三个子目录 |
| 流式解码状态机 | `decode/AudioStreamDecoder.*` | `IDLE/DECODING/SEEKING/EOS/ERROR`，还会懒初始化重采样器 |
| 编码器封装 | `decode/AudioDecode.*`, `decode/AudioDecodeProcess.*` | FFmpeg codec 与 extractor 输出在这里接上 |
| 重采样 | `resample/AudioResample.*` | `SwrContext` 生命周期、输入输出 `AudioSpec` 转换 |
| 设备输出 | `device/AudioDevice.*` | SDL 初始化、open/start/stop、回调取数 |
| 音频测试 | `../test/TestSdkSuite.cpp`, `../test/unit/test_audio*.cpp`, `../test/unit/test_decode.cpp` | 回归 + doctest |

## CONVENTIONS
- `sdk/audio/` 自己不做文件格式识别；输入元数据与数据源都应来自 extractor，别把 sniff/容器逻辑拉进来。
- `AudioStreamDecoder` 已用 `Optional<Lazy<std::shared_ptr<AudioResample>>>` 延迟创建重采样器；需要重置时跟随现有 Optional 赋空对象模式，不要改成裸指针缓存。
- `device/` 走 SDL 全局初始化与回调模型；任何改动都要同时考虑 `open → start → stop → close` 状态转换，而不是只看单个 API。
- `resample/` 依赖 `AudioSpec` 到 FFmpeg sample format/channel layout 的映射；新增格式支持时先补映射，再看测试。
- `decode/` 同时依赖 FFmpeg 与 `extractor` 暴露的 codec/spec；如果解码问题只在某个容器上出现，先排查 extractor 元数据是否正确。

## ANTI-PATTERNS
- 不要把 SDL 设备选择/驱动枚举当成已完成能力；`AudioDevice::getDeviceList()` 和 `selectDevice()` 目前还是 TODO 桩实现。
- 不要把 `AudioDevice` 的回调测试当成完全稳定的覆盖率证据；SDL 回调在线程里触发，覆盖率采样和时序都可能看不全。
- 不要在 `AudioResample` 重复 `init()` 后指望重新配置生效；当前实现遇到 `m_isInit` 直接返回。
- 不要绕过 `AudioStreamDecoder` 的状态轮询直接假设 seek/stop 已完成；现有测试都通过 wait/poll 辅助函数验证状态迁移。

## LOCAL CHECKS
```bash
cmake --build build --target TestSdkSuite
cmake --build build --target UnitTests
ctest --test-dir build -R sdk_resample_tests --output-on-failure
ctest --test-dir build -R sdk_decode_tests --output-on-failure
ctest --test-dir build -R sdk_unit_tests --output-on-failure
ctest --test-dir build -R sdk_player_tests --output-on-failure
```

## NOTES
- `sdk/test/unit/test_decode.cpp` 里对 `AudioStreamDecoder` 的验证依赖轮询辅助函数与 ring drain；改状态机时优先同步看这些测试，而不是只跑 smoke。
- `sdk/test/unit/test_audio_device.cpp` 对回调触发用了 `sleep_for(200ms)`，这是时序敏感测试；环境没音频设备时允许 `open()` 返回 `-1`。
- `decode/CMakeLists.txt` 额外依赖 `Vorbis`、`Ogg`、`avcodec/avformat/avutil`；`resample/` 依赖 `swresample`；`device/` 依赖 `SDL2`。这三个子目录的失败模式不同，排错时不要混成一类。
