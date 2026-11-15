# #165 调研资产：8 项遗留与 3 处口径偏差的事实真源与处置分级

- 时点：2026-11-15，master `ab5be374d7f6ff9b4c5f6a080b7396ae27ee8234`（`git rev-parse HEAD` 核实）。
- 方法：按 `research` skill，逐项追溯到拥有该事实的第一方源（源码 / 规格 / 票面 comment / 承接票正文），不采信 `docs/research/*.md` 的漂移行号。
- 边界：严格只读。未改仓库任何文件 / 规格 / 票正文 / 旧 resolution、未关票、未提交、未建分支、未 push。本文件写在仓库外临时目录。Grep/Glob 全部限定 path 并排除 `sdk/3rdparty`。未用 CodeGraph MCP。
- 组织口径：票面「8 项遗留」按承接票链 `#166`–`#172`（本地素材 t02–t08）与 #165 必答项还原；「3 处口径偏差」对应必答项 1/2/3 的记录口径核查。二者存在主题交叠（一项是「处置对象」，一项是「该对象的记录口径事实核查」），故分行列出、各自给判定。§11 界外项按约束只标注界外属性、不裁定。

---

## 一、处置分级表（8 项遗留 + 3 处偏差逐行）

| 项 | 结论（修·不修·已闭环·永久流程项·界外） | 真源落点 | 建议承接票名 | CI 触发面 |
|---|---|---|---|---|
| **L1** 稳定性闸门 40+30 轮对照状态 | **已闭环**（样本量证据齐备）；闸门纪律本身属**永久流程项**，非待修缺陷 | `AGENTS.md:129`（40 轮×2 各 39/40、定向 30/30）、`:125`（单用例连跑 100 次 0 崩溃）、`:133`（40 轮 39/40）、`:172`（40/40 全绿）；`#164` Out of scope「只裁定状态、不重跑」；`#172` 依据「`scripts/` 无脚本载体」 | 「裁定：永久流程项与已闭环项的最终处置」(#172) | 不触发（纯根 `AGENTS.md` / 票面结论） |
| **L2** `sdk/audio/decode/AudioStreamDecoder.cpp:1184` extradata 注释误述 | **修**（纯注释、零行为改动；当前**未修**，须与下次 `sdk/**` 代码提交同批，不单独提交） | 现值 `:1184-1185`「extradata 需要提取器新增访问 API」；反证 `:995` 已 `m_extractor->getCodecExtraData()`、`sdk/extractor/ExtractorHelper.hpp:139` 已有该虚方法；`#163` resolution「维持界外不实现，理由收益不足而非公开 API 边界」；`#163` 第七次审计 comment「唯一偏差：`AudioStreamDecoder.cpp:1184`」「只登记、不修」；`ab5be374d`/`#162` 只修了另外四处 | 「裁定：sdk 注释、ctest TIMEOUT 与分组注释漂移的处置」(#169) | `sdk/**` → 两个 workflow（sdk-windows + sdk-coverage） |
| **L3** `sdk/test/CMakeLists.txt` 前 7 个 `TestSdkSuite` 目标无显式 `TIMEOUT` | **修**（建议显式化现有默认值；**不得放宽**；当前**未修**，最终由 #169 裁定） | `:74-129` 共 7 个目标（core/resample/decode/player/all/media_smoke/extractor_spec），`set_tests_properties` 仅 `WORKING_DIRECTORY`+`ENVIRONMENT`，无 `TIMEOUT`（默认 1500s，见 `:217`「规格 10.#5」）；`:213/:228/:240/:252` 才有 `TIMEOUT 180/120/180/180` | 「裁定：sdk 注释、ctest TIMEOUT 与分组注释漂移的处置」(#169) | `sdk/**` → 两个 workflow |
| **L4** 两份 workflow 三处不对称 | **修**（推荐只修 `sdk-windows.yml:146` 误述注释、零行为变更；当前**未修**；缓存键/Linux 属性是否补由 #170 裁定） | (a) `sdk-coverage.yml:74-79` 同名 `Report coverage matrix numbers` 无 `continue-on-error`、无 `\|\| true`（门禁式）；`sdk-windows.yml:147-148` 有 `continue-on-error: true`（观测）；`:146` 注释「与 Linux 侧同名步骤的观测定位一致」与实际矛盾；(b) `sdk-windows.yml:65`/`:171` dist 缓存 `hashFiles(...)` 不含 `scripts/setup_windows_env.ps1`（脚本在 `:94` 调用）；(c) `sdk-coverage.yml` 全文无 `concurrency`/`timeout-minutes`，`sdk-windows.yml:37-39`/`:45` 有（150）。来源票 `#68`/`#92` | 「裁定：CI 双端同名步骤与缓存 / 并发属性的处置」(#170) | `.github/workflows/<自身>` 改动只触发对应一份（改 coverage 只触发 sdk-coverage，改 windows 只触发 sdk-windows） |
| **L5** 9 处「55 用例 / 396 断言」基线（实跑 398） | **修**（纯新增时点注记、既有行一字不动、`git diff --numstat`=`N 0`；当前**未修**，现值仍 396） | 9 处=`doc/bad-input-spec.md:110/154/663`、`doc/observability-contract.md:46/213`、`doc/network-audio-source-spec.md:193/242`、`doc/live-source-spec.md:388`、`docs/research/peak-gap-crosscheck.md:43`；实跑 398 见 `#118` comment `6891476606` 与 `#22`（`-ts=BadInput*` 两轮一致、四段 201+36+57+104=398；+2 来自 `7e85f5f13`）；定性「计数值过时、非违规改动、只登记不修」 | 「裁定：BadInput 396 断言基线的时点注记形态与落点」(#167) | 不触发（纯 `doc/` / `docs/`） |
| **L6** live mock 无窗口冻结端点（§10.1②「停发分片」既有覆盖缺口，现 6 例 / 100 断言） | **修**（先调研注入形态、再落判据；当前**未修**；若判「不可稳定注入」则按地图 Fog 条款转「不修·如实登记遗留」） | `sdk/test/mocks/HttpMocks.hpp:379/391/396/405/500/566/801` 仅按序号 fail/hang 端点（`setPlaylistFailWindow`/`setSegmentFailWindow`/`setSegmentHangWindow`/`setPlaylistHangWindow`/`holdOpenWithoutBody`），无 freeze/冻结端点；`sdk/test/unit/test_live_source.cpp:143` `TEST_SUITE("LiveSourceReconnect")`，`TEST_CASE` 计 6 个；`:211-216` 的 `frozen` 是局部 position 检查布尔、非 mock 端点；`doc/live-source-spec.md:289` §10.1② 三类形态；`#162` 口径更正 comment「该形态既无注入也无判据」 | 「调研：停发分片（冻结 playlist 窗口）注入形态与 hls 无包重载规避」(#166) /「执行：补 §10.1② 停发分片既有覆盖缺口」(#171) | `sdk/**` → 两个 workflow（#171 明确须与同批注释补漏合并提交） |
| **L7** 方向文件时效偏差（`doc/开发方向建议.md` + `doc/流式方案介绍.md`） | **不修**（维持「历史记录 + 不重写方向结论」；已登记、未修；方位词/枚举口径是否补注记由 #168 AI 推荐裁定） | `doc/开发方向建议.md:140-142`（「下方六个方向」方位与 `:5-138`/`:146` 实况相反、P1/P2/P3/P3-3 已落地使时点陈述过时）；`doc/流式方案介绍.md:3-19`（历史文档横幅、「至少八处已与当前代码不符」）；`#118` comment `6891136453` 三项发现 ②③「只登记、不修文档」 | 「裁定：方向文件选向与时效偏差的处置（AI 推荐裁定）」(#168) | 不触发（纯 `doc/`） |
| **L8** D25 第 6 项复核周期 | **永久流程项**（不修、不闭环、不随任何地图关闭） | `doc/live-source-spec.md:321`「复核周期：每次产品发布前至少一次 + 条款页变更时即时复核；这是流程要求，不是数值门禁 / 观测阈值」、`:312` §10.4；`#157` 归档 comment 第 26 行标永久流程项；`AGENTS.md:174`「不随本轮关闭」；`#164` Out of scope | 「裁定：永久流程项与已闭环项的最终处置」(#172) | 不触发（纯票面 / 文档结论） |
| **D1** 偏差① 稳定性闸门「未启动」表述 | **已闭环**（「未启动」**过时**；不需补对照；本图不重跑测量）；票面/**载体完整** | 见 L1：`AGENTS.md:125/129/133/172` 已载全套样本；`#164` Out of scope、`#172` 依据均为完整文字载体；`scripts/` 无脚本载体属预期（纪律不是脚本） | 「裁定：永久流程项与已闭环项的最终处置」(#172) | 不触发 |
| **D2** 偏差② 分组注释「现值 487 / 66.72s / 19」 | **不修**（票面所记「现值」**无第一方载体**，不可作同步依据；只登记、不回改）。若后续同步，真源**取 `sdk/test/CMakeLists.txt:185-192`+`:251` 这一组** | 精确检索 `66\.72`、`487 例/487 个用例/ /487 / 487 /` 在 `doc/*.md`、`sdk/test` 源码（`*.txt/cmake/cpp/hpp/h/md`）、`docs/*.md` 全部**零匹配**（`docs/research/` 命中的 34874/34737 是 asf 时长无关数字）；真源组：`:186` 全量 184 s、`:187` StreamingBudget 59 s/24 例、`:188` 其余合计 30 s/413 例、`:189` 坏输入 55 例 94 s、`:190`/`:216` 单段 24/27/15/28 s、`:251` 交互 14 例/11.8 s、`:212` 快集约 30 s、`:239` 实测 59 s | 「裁定：sdk 注释、ctest TIMEOUT 与分组注释漂移的处置」(#169) | 若同步注释属 `sdk/**` → 两个 workflow |
| **D3** 偏差③ `doc/流式方案介绍.md` 时效偏差的登记载体 | **不修**（**有其它第一方载体，不是本图新登记项**；维持既有「历史记录 + 不重写」处置） | 详见下节 §三-3。`#118` comment `6891136453` 确只登记 `doc/开发方向建议.md`、未提本文档；但本文档偏差早有载体：`doc/流式方案介绍.md:3-19` 历史横幅（指向「#43 的 resolution comment」+ `#59` 补充）、`doc/streaming-playback-spec.md:178` E14 行「`doc/流式方案介绍.md` 偏差清理 \| #51 / #53」、`#53` 收口记录已做「标注：顶部加历史文档横幅 + 指向新规格，正文不重写」 | 「裁定：方向文件选向与时效偏差的处置（AI 推荐裁定）」(#168) | 不触发（纯 `doc/` / 票面） |

**界外附注（不裁定，只标注属性）**：P4 DASH（`CONFIG_DASH_DEMUXER 0`，两端恒定）、公网电台进 CI / 产品默认列表、HTTPS 入口 —— 依 `doc/network-audio-source-spec.md` §11 / `doc/live-source-spec.md` §11 / `#164` Out of scope / `#172` 决策范围 3，均为**界外**；本票不重开、不裁定，只有重画 Destination 才作为新 effort 回归（`#172`）。

---

## 二、CI 触发面统一依据（必答项 5）

两份 workflow 的 `push`/`pull_request` `paths` 均只列 `CMakeLists.txt` / `cmake/**` / `app/**` / `scripts/**` / `sdk/**` / `.github/workflows/<自身>.yml`，**不含** `doc/` / `docs/` / 根 `AGENTS.md` / `GLOSSARY.md`：

- `sdk-coverage.yml:10-16`（push）、`:18-24`（PR）、`:16`/`:24` 只列 `sdk-coverage.yml` 自身；
- `sdk-windows.yml:20-26`（push）、`:28-34`（PR）、`:26`/`:34` 只列 `sdk-windows.yml` 自身；
- `AGENTS.md:121` 亦明确「纯 `doc/` / `docs/` / `AGENTS.md` 提交不触发 CI」。

故：纯 `doc/` / `docs/` / 根 `AGENTS.md` → 0 个 workflow；`sdk/**` → 2 个（sdk-windows + sdk-coverage）；`.github/workflows/<自身>` → 1 个（仅对应那份）。成败只认 `gh run view <id> --json status,conclusion,attempt` 的 attempt 1 `success`（`#164` CI 纪律）。

---

## 三、三处口径偏差的逐项裁定依据

### D1 稳定性闸门 40+30 轮（必答项 1）

- **「未启动」表述过时**：闸门样本在 `AGENTS.md` 已全部落地 —— `:129`「`ctest -R sdk_ -j4` 各 40 轮，分支与 master 都是 39/40」+「定向单跑 `-R ^sdk_interaction_tests$` 30/30」；`:125`「单用例连跑 100 次、0 崩溃 0 非零退出（127s）」；`:133`「本机 40 轮 39/40 = 基线 39/40」；`:172`「本机 40 轮 `ctest -j4` **40/40 全绿**（优于基线 39/40）」。票面把这些一并归到 `:129`，实为分散在 `:125/:129/:133/:172` 多行，但不影响结论。
- **「已闭环」而非「仍需补对照」**：40 轮同轮数对照 + 失败目标定向 30 轮是 `#172` 依据所称的「稳定性闸门**样本量纪律**，非待修缺陷」；实测已完成，`#164` Out of scope 明确「只裁定其状态，不重跑测量（`AGENTS.md:129` 已有样本）」。
- **但属永久流程项**：每次新增测试钩子 / 测试专用钩子仍须走同轮数对照（`AGENTS.md:124`），不是一次性关闭的缺陷；`#172` 承接其终局处置。
- **票面已有完整载体**：是。`AGENTS.md` 多行 + `#164` Out of scope + `#172` 依据构成完整文字载体；`scripts/` 无脚本载体属设计预期（纪律非脚本），无需另补。

### D2 分组注释数值（必答项 2）

- **票面所记「现值 487 / 66.72s / 19」无第一方载体**：精确 Grep `66\.72`、`487 例|487 个用例|/ 487|487 /` 在 `doc/*.md` → No matches；`sdk/test`（源码文件类型，排除 `.log`）→ No matches；`docs/*.md` → 仅命中 `docs/research/observability-facts.md:75/:79`、`observability-calibration.md:111` 的 `34874/34737`（asf 时长，与分组注释无关）。票面自己与 `#169` 依据均记「全仓零匹配」，核实一致。
- **真源应取哪一组**：`sdk/test/CMakeLists.txt:185-192` + `:251`（+ `:212/:216/:239` 佐证）= 全量 184 s、StreamingBudget 59 s / 24 例、其余合计 30 s / 413 例、坏输入 55 例 94 s、单段 24/27/15/28 s、交互 14 例 / 11.8 s、快集约 30 s。**不是** 487 / 66.72s / 19。
- **处置**：「现值」口径作废、`不修`（只登记、不回改）；若 `#169` 后续裁定同步分组注释，只能同步为上述源码现值组。

### D3 `doc/流式方案介绍.md` 时效偏差的登记载体（必答项 3）

- `#118` 第 4 条 comment（`6891136453`，已调取全文核实）确为「只读核对结果登记 —— §12.3 文档接缝表」，三项发现 ②③ 只涉及 `doc/开发方向建议.md`，**未提及 `流式方案介绍.md`**；结论「只登记、不修文档」。
- **但本文档偏差早有其它第一方载体，不是本图新登记项**：
  1. `doc/流式方案介绍.md:3-19` 自身有「历史文档（2026-11-05 标注）」横幅，明列「至少八处已与当前代码不符」（`rawBuffer` 已删、`start(AudioBufferPtr)` 改签名、重采样不在 `AudioDecodeProcess`、SDL 实为 push 等），`:13` 指向「偏差清单见 #43 的 resolution comment」，`:15-18` 有 `#59` 后续偏差补充 —— 偏差已被文档自身如实登记为历史文档。
  2. `doc/streaming-playback-spec.md:178` 实现清单 **E14**（P2）明列「`AudioRingBuffer.h:7-9` 注释更新、`doc/流式方案介绍.md` 偏差清理 \| #51 / #53」—— 已作为 P2 实现清单项登记。
  3. `#53`「汇总成流式播放规格与 ADR」收口记录含「标注 \| `doc/流式方案介绍.md` \| 顶部加『历史文档』横幅 + 指向新规格；正文不重写，消除两套口径并存」—— 已做「标注为历史文档」的最小处置。
  4. `#168` 决策范围 3「`流式方案介绍.md` 的偏差是否本图新登记、以何种形态承载」是承接票待裁项；本调研给出事实：既有载体齐全，裁定应落「不修·非新登记项」。
- **未确认**：`#43` resolution comment 全文未直接调取（依 `doc/流式方案介绍.md:13` 指向认定其为偏差清单载体）；不影响「已有其它载体」结论。

---

## 四、未修 / 已修 结论速查（必答项 4）

| 落点 | 状态 | 精确现值 |
|---|---|---|
| `AudioStreamDecoder.cpp:1184-1185` extradata 注释 | **未修** | 仍写「extradata 需要提取器新增访问 API」；`#163` 第七次审计 comment 已「只登记、不修」 |
| `CMakeLists.txt:74-129` 前 7 个 `TestSdkSuite` 目标 | **未修** | 无显式 `TIMEOUT`（默认 1500s）；`:213/:228/:240/:252` 才有 |
| 两份 workflow 三处不对称 | **未修** | 门禁属性不对称 / 缓存键缺 setup 脚本 / Linux 缺 `concurrency`+`timeout-minutes`，四处落点见 L4 |
| 9 处「55/396」基线 | **未修** | 9 处现值仍 396；实跑 398（`#118` comment `6891476606`） |
| live mock 窗口冻结端点 | **未修** | `HttpMocks.hpp` 无 freeze 端点；`LiveSourceReconnect` 6 个 `TEST_CASE`（`test_live_source.cpp:143` 起） |
| D25 第 6 项复核周期 | **永久流程项**（非待修） | `live-source-spec.md:321` 原文完整、`AGENTS.md:174` 不随本轮关闭 |

---

## 五、未确认项与边界声明

- 「现 6 例 / **100 断言**」：6 例已由 `TEST_CASE` 计数核实；**100 断言沿用 #166 / #171 票面声明，未独立复跑 doctest 计数**（本票只读、不跑测试）。
- 「8 项遗留」的单一原始清单文档未找到（临时目录 t01–t08 是承接票正文、`map_body.md` 是 #164 母票；仓库内无独立「8 项清单」文件）；本表按承接票链 #166–#172 与 #165 必答项还原，覆盖票面全部裁定对象。
- `#43` resolution 全文未直接调取（见 D3）。
- `docs/research/*.md` 一律按时点快照处理，冲突以源码与规格为准；本表所有 file:line 均取自当前 master 实读。
- 未裁定 §11 界外项（P4 DASH / 公网电台 / HTTPS），只在分级表附注标界外属性。
- 全程严格只读：未修改仓库任何文件、未改规格、未改票正文 / 旧 resolution、未关票、未提交、未创建分支、未 push。
