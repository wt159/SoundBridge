# AGENTS.md

## 项目概览
SoundBridge 是一个使用 C++ 和 Qt 构建的跨平台本地音乐播放器。
它提供模块化 SDK，用于解码、重采样和播放，并包含一个链接 SDK 的 Qt UI 应用。
项目目标平台为 Windows、Linux 和嵌入式 Linux。

## 仓库结构
- `app/` Qt UI 应用（入口：`app/main.cpp`，UI：`app/mainwindow.cpp`）
- `sdk/` 核心 SDK（播放器、解码、重采样、设备、提取器、日志）
- `sdk/audio/` 音频流水线模块（解码/设备/重采样/通用）
- `sdk/log/boost_log/` 日志封装（Boost.Log）
- `sdk/extractor/` 媒体元数据/提取
- `sdk/utils/` 共享工具
- `sdk/test/` SDK 测试（`TestSdkSuite`）
- `sdk/3rdparty/` 第三方构建脚本与预构建 `dist/`
- `cmake/` 工具链与构建辅助
- `scripts/` 辅助脚本（Windows 测试运行器）
- `music/` 示例媒体文件（测试使用）
- `package/` 便携打包输出
- `build/` 本地构建输出（生成目录）

## 技术栈
- 语言：C++
- UI：Qt5 Widgets
- 构建：CMake
- 音频解码：FFmpeg（avcodec/avformat/avutil）+ FLAC + Ogg/Vorbis
- 音频输出：SDL2
- 日志：Boost.Log
- 文件系统：Boost.Filesystem

## 架构（SDK）
分层由 CMake 链接依赖约束：
- `sdk/` 公共 API：`MusicPlayer`、`MusicPlayList`、`LogApi`、`Metrics`、`ErrorCode`
- `sdk/audio/decode/` 解码（FFmpeg、FLAC、Ogg/Vorbis）
- `sdk/audio/resample/` 重采样（swresample）
- `sdk/audio/device/` 播放设备（SDL2）
- `sdk/extractor/` 元数据与提取
- `sdk/log/boost_log/` 日志后端
- `sdk/utils/` 共享工具

Agents 不得在这些层之间引入循环依赖。

## 构建前置条件
1. 先构建第三方依赖（输出到 `sdk/3rdparty/dist/<platform>`）。
2. 使用 `cmake/toolchain/` 中正确的工具链进行配置。
3. 确保 CMake 可发现 Qt5 和 SDL2。

依赖构建步骤见 `sdk/3rdparty/Readme.md`。

## 构建说明
### Windows（MinGW）
1. `cd SoundBridge`
2. `mkdir build && cd build`
3. `cmake .. --no-warn-unused-cli -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=..\cmake\toolchain\toolchain.windows_x86_64_mingw.cmake -G "MinGW Makefiles"`
4. `cmake --build .`

### Linux（x86_64）
1. `cd SoundBridge`
2. `mkdir build && cd build`
3. `cmake .. --no-warn-unused-cli -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain/toolchain.linux_x86_64_gcc.cmake -G "Unix Makefiles"`
4. `cmake --build .`

### 嵌入式 Linux（arm-linux-gnueabihf）
1. `cd SoundBridge`
2. `mkdir build && cd build`
3. `cmake .. --no-warn-unused-cli -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain/toolchain.linux_arm_gnueabihf_gcc.cmake -G "Unix Makefiles"`
4. `cmake --build .`

## 打包（Windows 便携版）
在 `build/` 目录成功构建后：
1. `cmake --build . --target package_portable`

可选配置（仅首次配置时）：
- `SOUNDBRIDGE_PACKAGE_DIR`
- `SOUNDBRIDGE_WINDEPLOYQT`

便携输出默认位于 `package/SoundBridge_portable_v3`。

## 测试
目标与 ctest 条目定义在 `sdk/test/CMakeLists.txt`。

运行 SDK 测试：
1. `cmake --build . --target TestSdkSuite`
2. `ctest -R sdk_ --output-on-failure`

Windows 辅助脚本：
- `scripts/run_sdk_tests.ps1` 负责准备运行时目录并执行测试套件。

媒体冒烟测试使用 `music/` 样本：
- `ctest -R sdk_media_smoke --output-on-failure`

## 生成物
- `build/` 为生成目录，可安全删除。
- `package/` 包含打包输出二进制文件。

## Agent 指南
- 保持 SDK 模块边界完整；优先在最低合适层添加新功能并向上暴露。
- 避免编辑 `sdk/3rdparty/` 下的内容，除非更新依赖源码。
- Windows 工具链路径应通过 CMake 缓存变量保持可配置。
