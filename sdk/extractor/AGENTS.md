# EXTRACTOR KNOWLEDGE BASE

## OVERVIEW
`sdk/extractor/` 是格式提取器族：工厂按扩展名与 magic sniff 选择具体实现，输出统一的音频元数据/流入口。

## STRUCTURE
```text
sdk/extractor/
├── ExtractorFactory.*        # 注册表、扩展名规范化、magic sniff
├── DataSource.hpp            # 数据源抽象
├── FileSource.*              # 本地文件数据源
├── FileSearch.*              # 文件搜集
├── asf/                      # ASF/WMA/AMR 共享实现；看 asf/AGENTS.md
└── {aac,aiff,ape,flac,m4a,mkv,mp3,ogg,wav}/
                           # 每种格式一组 extractor 实现
```

## AGENTS HIERARCHY
- `asf/AGENTS.md`：ASF/WMA/AMR 三扩展共享实现、FFmpeg demux 与手写 packet 解析双路径。

## WHERE TO LOOK
| 任务 | 位置 | 备注 |
|------|------|------|
| 新增格式支持 | `ExtractorFactory.cpp` + 新格式子目录 | 先注册，再补 sniff/creator |
| 统一数据源语义 | `DataSource.hpp`, `FileSource.*` | `readAt/getSize/getUri` 约束都在这里 |
| 扩展名或 magic 识别 | `ExtractorFactory.cpp` | `normalizeExtension()` / `sniffExtensionByMagic()` |
| FFmpeg demux 初始化 | `asf/`, `ape/`, `m4a/`, `mkv/` 等 | 多数复杂格式在这里 |
| extractor 回归 | `../test/unit/test_extractor.cpp`, `../test/TestSdkSuite.cpp` | 单测 + 媒体冒烟 |

## CONVENTIONS
- 新 extractor 必须保持工厂入口一致：构造函数接 `DataSourceBase *`，并暴露 `initCheck()`；能 sniff 的格式优先补 sniff。
- `ExtractorFactory` 先看扩展名，再按 magic，再遍历 sniffer；改顺序会影响兼容性与日志判断。
- 复杂容器优先在本目录消化格式差异，不要把 FFmpeg 细节泄漏到上层 player/app。
- 本目录内部继续用 `sdk_utils::status_t`，并按既有 `LOGI/LOGW/LOGE` 路径记录失败原因。

## ANTI-PATTERNS
- 不要在 `avformat_close_input()` 之后再读 `fmt->duration` 等字段。
- 不要新增 extractor 但漏掉 `ExtractorFactory.cpp` 注册；那样测试只会返回 unsupported extension。
- 不要把路径编码转换散落到各 extractor；跨平台路径处理应收敛到 `FileSource`/utils 边界。

## LOCAL CHECKS
```bash
cmake --build build --target TestSdkSuite
cmake --build build --target UnitTests
ctest --test-dir build -R sdk_extractor_spec_tests --output-on-failure
ctest --test-dir build -R sdk_media_smoke --output-on-failure
ctest --test-dir build -R sdk_decode_tests --output-on-failure
```

## NOTES
- `sdk/extractor/CMakeLists.txt` 通过 `file(GLOB_RECURSE "*.cpp")` 收源；新 `.cpp` 文件通常无需手动登记，但新增头/目录仍要确认构建与注册链都通。
- 支持扩展名并不总是“一目录一实现”：`ExtractorFactory.cpp` 里 `.wma`、`.amr` 目前复用 `ASFExtractor`。
- `mp3/ID3.*` 与 `FileSearch.*` 也在这个模块边界内，不要误判成 `sdk/utils/` 职责。
