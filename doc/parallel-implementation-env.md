# 并行实现的环境与协议（坏输入 effort，历史归档）

> **历史文档（2026-11-07 归档）**
>
> 本文件原为 `.worktrees/_shared/ENV.md`，是 2026-11-03 坏输入地图（#22）并行实现期间，
> 给所有 implementer 共用的环境协议。相关 worktree 目录即将清理，原文若不归档会随目录一起丢失。
>
> **分支名与目录已经过时**：`feat/bad-input-spec`、`bad-input/<票号>` 分支和
> `.worktrees/t34`、`t35`、`t37` 等目录均已合并或废弃，不要照第三节的命令直接执行。
> **第三节「五个坑」仍然有效**，跨平台并行开发时最容易踩。
>
> 当前构建与测试命令以根 `README.md` / `AGENTS.md` 为准；测试分组见
> `sdk/test/CMakeLists.txt`（2026-11-06 起按耗时拆为四组 ctest）。

## 哪些还有效（2026-11-07 复核）

| 条目 | 状态 |
|---|---|
| `dist` 需要目录联接（worktree 里没有 3rdparty 构建产物，缺了 config 直接 `FATAL_ERROR`） | 仍有效。**反向风险已确认**：`t34/t35/t37` 的 `sdk/3rdparty/dist` 是指回主仓库的 junction，用资源管理器或 `rm -rf` 删目录会跟进删掉主仓库的 `dist` |
| 配置时的 `PATH` 会被烘焙进 `add_test`（`sdk/test/CMakeLists.txt` 的 `SDK_TEST_PATH`） | **仍有效**。改了 PATH 必须重新跑一次 `cmake -S . -B build`，否则测试报 `0xc0000135`，而手动跑同一个 exe 却是好的 |
| `ctest` 不在 PATH 里，要用绝对路径 | 仍有效 |
| Git Bash 里 `./UnitTests.exe` 偶发 `exit=127` 且 stdout 全空 | **仍有效**，那不是通过，是没跑起来 |
| 用例名用英文（GBK 控制台下中文 `-tc=` 会静默 0 匹配） | 仍有效 |
| 建联接用 PowerShell，不要 `cmd //c` / `mklink` | 仍有效 |
| `-j4` 而不是 `-j8`（多个 implementer 并行编译） | 视并行度而定，单人开发可以 `-j8` |

已过时：集成分支名、`ctest -R sdk_unit_tests` 跑全量（现在是 14 个目标：7 个 `TestSdkSuite` 分组 +
快集 / 坏输入四段 / `sdk_streaming_tests` / `sdk_interaction_tests`，2026-11-07 起 `sdk-windows.yml`
用 `ctest -j4` 跑）、`.worktrees/_shared/` 共享笔记机制。

---

以下为原文，除本节外未改动。

# SoundBridge 坏输入实现：环境与协议

所有 implementer 共用这一份。先读完再动手，这里的每一步都已经实跑验证过。

## 绝对路径

| 用途 | 路径 |
|---|---|
| 主仓库 | `D:\workspace\SoundBridge` |
| **集成分支** | `feat/bad-input-spec` |
| 规格（唯一真源） | `D:\workspace\SoundBridge\doc\bad-input-spec.md` |
| 共享笔记（可读可写） | `D:\workspace\SoundBridge\.worktrees\_shared\` |
| 你的 worktree | `D:\workspace\SoundBridge\.worktrees\t<票号>` |

## 一、建立你的 worktree

```bash
cd /d/workspace/SoundBridge
git worktree add .worktrees/t<票号> -b bad-input/<票号> feat/bad-input-spec
```

`dist` 被 `.gitignore` 排除，worktree 里没有第三方库，而根 `CMakeLists.txt` 找不到它会直接 `FATAL_ERROR`。
用目录联接指回主仓库（**必须**，否则配置就挂）：

```powershell
New-Item -ItemType Junction -Path "D:\workspace\SoundBridge\.worktrees\t<票号>\sdk\3rdparty\dist" -Target "D:\workspace\SoundBridge\sdk\3rdparty\dist"
```

## 二、配置与构建

**配置命令必须带 `PATH` 和 `QT_ROOT_PATH`**（原因见第三节第 2 条）：

```bash
cd /d/workspace/SoundBridge/.worktrees/t<票号>
export PATH="/d/Qt/Tools/mingw730_64/bin:/d/Qt/Qt5.14.2/5.14.2/mingw73_64/bin:$PATH"
export QT_ROOT_PATH=D:/Qt
D:/tools/cmake-3.31.6-windows-x86_64/bin/cmake.exe -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=D:/workspace/SoundBridge/.worktrees/t<票号>/cmake/toolchain/toolchain.windows_x86_64_mingw.cmake \
  -DCMAKE_MAKE_PROGRAM=D:/Qt/Tools/mingw730_64/bin/mingw32-make.exe \
  -G "MinGW Makefiles"
```

构建（全量约 2 分钟，增量约 3 秒）：

```bash
D:/tools/cmake-3.31.6-windows-x86_64/bin/cmake.exe --build build -j4
```

`-j4` 是**故意的**：机器 8 核，同时会有两三个 implementer 在编译，别用 `-j8` 互相抢。

## 三、测试

```bash
D:/tools/cmake-3.31.6-windows-x86_64/bin/ctest.exe --test-dir build -R sdk_unit_tests --output-on-failure
```

单跑一个套件（快）：

```bash
cd build/sdk/test && ./UnitTests.exe -ts=BadInput
```

**五个坑，踩了会浪费半小时：**

1. **`dist` 联接**：见第一节。缺了报 `3rdparty/dist/... not exist, please build 3rdparty`。
2. **配置时的 `PATH` 会被烘焙进测试命令**。`sdk/test/CMakeLists.txt:62-65` 把 `$ENV{PATH}` 拼进 `SDK_TEST_PATH`，`add_test` 又用 `cmake -E env "PATH=..."` **替换**整个 PATH。所以配置时 PATH 里没有 Qt 的两个 bin，测试就会以 `0xc0000135`（找不到 DLL）失败——而**手动跑同一个 exe 却是好的**，极易误判成代码问题。改了 PATH 后要重新跑一次 `cmake -S . -B build` 让它重新烘焙。
3. **`ctest` 不在 PATH 里**，用上面的全路径。
4. **`cmd //c`、`mklink` 在 Git Bash 里不可靠**，建联接用 PowerShell 工具，不要 `cmd //c`。
5. **Git Bash 里 `./UnitTests.exe` 偶发 `exit=127` 且 stdout 全空**——那不是测试通过，是没跑起来。遇到空输出换 PowerShell 工具重跑。

## 四、改代码前必须知道的项目约定

- **不改 `sdk/3rdparty/`**，一行业务问题都不要通过改供应商代码解决。
- **C++ 用例名用英文**：中文名在 GBK 控制台下 `-tc=` 过滤会静默 0 匹配。
- 错误分两层：内部 `sdk_utils::status_t`，对外 `soundbridge::ErrorCode` / `sdk::ErrorCode`。
- 编码/路径转换集中在 `sdk/utils/Utf8Path.h`，不要往业务代码里散 `#ifdef _WIN32`。
- 仓库用 CRLF 行尾，改写已有文件时不要改行尾风格。

## 五、TDD 怎么做

1. 先写一个**当前会红**的用例，红的原因必须是「缺陷还在」，不是「还没实现」。
2. 素材用**字节手术在临时目录合成**，不入库、跑完删；读源走 `FileSource`（`music/` 里有中文名文件）。
3. 跑到绿，再跑整套 `ctest -R sdk_`，确认没打翻别的。
4. 判据看规格第 2 节总表与各节「判据」小节，**不要自己发明容差**。

## 六、完成协议

1. 只改你这张票覆盖的东西。碰到票外的缺陷，记进 `_shared/findings-<票号>.md`，不要顺手改。
2. 提交到自己的分支 `bad-input/<票号>`，信息用 `fix:` / `feat:` 前缀 + 中文分点。
3. **报告完成前先 `git merge feat/bad-input-spec`**，把集成分支的最新进展并进来，冲突自己解。
4. **不要 push 远端**，也不要碰 `feat/bad-input-spec` 本身——合并由编排方做。
5. 报告里写清楚：改了什么文件、新用例叫什么、跑过什么命令、结果如何。不要复述规格。
