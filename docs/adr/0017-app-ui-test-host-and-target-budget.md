# 0017 UI 测试宿主抽 app_core STATIC，第 15 个 ctest 目标获一次性例外

- 状态：已接受
- 日期：2026-11-22
- 来源：[地图：app 层 UI 判据全量自动化](https://github.com/wt159/SoundBridge/issues/248)、[测试宿主的构建形态与覆盖率分母](https://github.com/wt159/SoundBridge/issues/251)、[第 15 个 ctest 目标的注册口径与纪律例外](https://github.com/wt159/SoundBridge/issues/252)、[CI 双端能力取证](https://github.com/wt159/SoundBridge/issues/249)。规格落点 [`doc/app-ui-spec.md`](../../doc/app-ui-spec.md) §9.7.1 / §9.7.3 / §9.7.4。

## 背景

app 源（`mainwindow` / `playercontroller` / `popuppanel`）目前只编进 `SoundBridge.exe` 一个目标（`app/CMakeLists.txt` 的 `add_executable(${PROJECT_NAME} WIN32 ${APP_SOURCES})`）。测试要链接它们，两条路：抽静态库，或测试目标重复编源——[可行性研究](../research/app-ui-test-feasibility.md)量过两条路的构建代价。

目标数纪律：`ctest -R sdk_` 共 14 个目标（分组真源 `sdk/test/CMakeLists.txt:185-192`，记载于 `sdk/test/AGENTS.md`「CTest 分组与并行」）。`TIMEOUT` 现有档位：老 `TestSdkSuite` 目标 1500、坏输入四段 120（实测 24/27/15/28 s 的 4 倍余量）、快集/交互 180。

覆盖率分母：`scripts/coverage/run_linux.sh:45-49` 用 `find "${ROOT_DIR}/sdk"`（排除 `test/`、`3rdparty/`）构造 `lcov --extract` 清单——app 源天然不在分母，门禁 33%。

CI 能力（#249）：Windows `qtbase` 归档含 `Qt5Test.dll`、`Qt5TestConfig.cmake`、`qoffscreen.dll`（已实测）；Linux `qtbase5-dev` 依赖链拉齐 `libqt5test5t64` 与 `libqt5gui5t64`（后者提供 `libqoffscreen.so`）（元数据可证）。双端都**不需要新增安装步骤**。

## 决策

1. **构建形态：抽 `app_core` STATIC 库。** 三个 app 源文件及其 `AUTOMOC` / `AUTOUIC` / `AUTORCC` 迁入 `app_core`；`SoundBridge.exe` 改链 `app_core`；测试可执行 `AppUiTests` 链 `app_core + Qt5::Test + sdk`。源只编一次，moc 产物与 qrc 资源单份。
2. **目标注册：`sdk_app_ui_tests`（可执行 `AppUiTests`）注册在 `app/CMakeLists.txt`、`app_core` 之后。** 根 `CMakeLists.txt:6` 的 `include(CTest)` 先于各 `add_subdirectory`，`add_test` 生效；保留 `sdk_` 前缀使目标仍被 `-R sdk_` 伞命中，「整轮 ctest」纪律不断裂。**目标数 14 → 15 为一次性显式例外**：已在 `sdk/test/AGENTS.md` 登记；此后任何新增目标需另开例外，不自动延展。
3. **预算：`TIMEOUT 120`**（沿用坏输入四段档位值，不新增第四档位）；`ENVIRONMENT` 设 `QT_QPA_PLATFORM=offscreen` 与 `SDL_AUDIODRIVER=dummy`。
4. **覆盖率分母不变。** 不把 `app/**` 加入 `run_linux.sh` 的 extract 清单，33% 门禁与其「sdk 业务源」语义不变；app 层质量由 `AppUiTests` 断言承担，不由行覆盖承担。
5. **部署与稳定性注记。** Windows 下 `platforms/qoffscreen.dll` 不在 `windeployqt` 默认部署集，必须随测试 exe 显式部署——#249 实测缺它时进程停在 `Could not find the Qt platform plugin "offscreen"` 致命对话框，不返回、不写日志，`timeout` 只见退出码 124。测试内部守卫必须宽于被测链路的超时预算（#140 先例：2s → 15s）。

## 考虑过的替代方案

**测试目标重复编 app 源。** 构建时间近乎翻倍，`AUTOMOC` 产物两份易漂移（同一个 `mainwindow.h` moc 出两个元对象），否决。

**独立 workflow 跑 UI 测试、不入 ctest 门禁。** 红不阻合并的测试必然腐烂，与「全量自动化」目的地冲突，否决。

**进 ctest 但 `continue-on-error` 宽限。** 只观测不阻断等于没有门禁，且与既有目标的语义不一致，否决。

**`TIMEOUT 1500` 照抄老档。** 与秒级实际耗时严重错配，挂死检测退化为摆设，否决。

**目标名 `app_ui_tests`（去 `sdk_` 前缀）。** `ctest -R sdk_` 伞与「整轮 ctest」纪律被劈开，两套命令口径，否决。

## 后果

- `app/CMakeLists.txt` 增加库目标与测试目标；双端 CI 均无需新增安装或环境步骤。
- 覆盖率报表保持 sdk-only 语义；若将来要度量 app 层行覆盖，需另开 ADR 重定分母与门禁。
- 判据索引与「能红」纪律见规格 §9.7.2 / §9.7.4；实现由图外执行票落地。
