# O3 · 格式内长短配对峰值交叉校验：验证步骤（L3 非门禁）

> 本节数值只作观测记录，**不作为失败门禁**；记录失败（素材缺失 / 命令异常 / 无输出）不得让 CI 失败，也不得被解读为判据通过。
>
> 本文是 `doc/observability-contract.md` §6 / §8.3 图外项 **O3** 的产出（票 [#107](https://github.com/wt159/SoundBridge/issues/107)），**只给验证步骤与已归档数值，不给阈值、不给红绿结论、不新增任何可红可绿判据**。
> 状态口径：**L3 待验 / 格式内交叉校验（增强项）**；§6 的跨格式峰值差判据属 **L0 已通过**（#67 明写 StreamingBudget 四条判据全绿），本文不改变其状态，也不重开 #48 常驻上界判据。

## 1. 为什么落成文档而不是用例

1. `sdk-coverage.yml:91-96` 的观测 step 只 grep 六个关键字；任何含 `peakHeldBytes` / `packets` 的新 `MESSAGE` 行都会让 O5 归档的「每 run 24 行 `peakHeldBytes`」基线变成 25 行，属于改动已归档基线。
2. `sdk/test/unit/test_streaming_budget.cpp:302-304` 明写「不新增一次长素材播放（不新增耗时 gate）」；为交叉校验再跑一遍长素材违反该约束。
3. O5 的 13 个成功 master push run 里，峰值结构值跨运行完全恒定（`distinct=1`），配对所需数值**已经齐备**（见 `docs/research/observability-calibration.md` §3），无需重新采集。
4. §3.2 第 5 步经 #104 裁决已**永久生效**：不得写入断言、不得注册 `add_test` 分组、不得把观测 step 改成门禁。

## 2. 验证步骤（人工复跑，不注册 `add_test`）

1. **取 CI 数值**：`sdk-coverage` 的 `Report streaming budget instruments` step（`sdk-coverage.yml:91-96`）跑 `UnitTests -ts=StreamingBudget -s | grep -E "<六个关键字>"`；step 带 `continue-on-error: true`（步骤级）与 `|| true`（命令级）双保险，两者覆盖不同失败面，缺一不可。
2. **本地复核**：按项目约定的三重 PATH（3rdparty `bin` + Qt `mingw73_64/bin` + `mingw730_64/bin`），cwd 用 `sdk/test`，跑 `UnitTests -ts=StreamingBudget -s`；对照 `test_streaming_budget.cpp:337-340` 的 `held-peak gap (same cross-format pair)` 行与各格式的 `MESSAGE` 行。
3. **取按格式分组的数值**：`docs/research/observability-calibration.md` §3 表（13 run 的 `min..max`，全部「恒定」）。素材形状见 `docs/research/observability-facts.md` 表 3 与 `test_media_coverage.cpp:36-50`。
4. **配对求差**：按扩展名把长短两首的 `peakHeldBytes` 相减，登记 `|Δ|` 与时长形状；**不判 `≤ P`、不写 `CHECK` / `REQUIRE`、不写「通过 / 失败」**。
5. **留痕**：把 `|Δ|`、取数 run id 与日志行号追加到本文件对应行，供同一实现内纵向比较；跨运行差异只记录，不追责、不设门槛。

## 3. 现有数值与配对

| 配对 | 短侧（`peakHeldBytes`） | 长侧（`peakHeldBytes`） | `|Δ|` (B) | 取数场景 |
|---|---|---|---|---|
| `.aac` 格式内 | `48000_fltp_1.aac` 10.048 s → `20539` | `WavinFlag.aac` 220.984 s → `25239` | `4700` | 短侧只有 **seek 场景**（delivered `8498` / declared `10048` ms），非稳态 whole-track |
| `.m4a` 格式内 | `music-alac.m4a` 12.000 s → `30434` | `摇滚乐_Freesound.m4a` 69.892 s → `17354` | `13080` | 同扩展名但**不同编码**（ALAC vs AAC） |
| 跨格式对照（既有 L0 用例） | ogg `3.709 s` → `4608` | wma `259.164 s` → `33325` | `28717` | `test_streaming_budget.cpp:334-340`，§6 已判通过 |

数值来源：`docs/research/observability-calibration.md` §3「按格式分组的结构值」；ogg / wma 的时长与峰值同表，wma 的 measured `261642` / declared `259164` ms 差异属既有 1% 容差范围，与本文配对无关。

## 4. 口径限制（必须显式登记）

1. **aac 短侧不是稳态值**：24 行归档里 `48000_fltp_1.aac` 只出现在 seek 场景，与长侧的 whole-track 取数口径不自洽，不能据此谈「同格式长短差」。
2. **m4a 的「格式内」只到扩展名层级**：两首编码不同（AAC / ALAC，`doc/format-support.md:27`），与 §6 讨论的「同格式」不是同一个概念，避免把本表读成 §6 判据的反例或补证。
3. **`peakHeldBytes` 不是时长函数**：它是「本次播放为当前曲目持有的音频数据字节数」，不含 ring / AVIO 缓冲（`StreamMetrics.h:14-19`）；量级由解码器工作缓冲与包形决定，与素材时长没有线性关系。
4. **禁止跨源比较**：`initScanUs`(µs) 与 `open_file_ms` / `track_switch_ms`(ms) 不是同一量的两次测量（规格 §5），本文只用 `peakHeldBytes`(B)，不与任何 µs / ms 数值相减或换算。
5. **不拍任何数字**：#104 已裁「维持只记录、不设任何数值阈值」，§3.2 第 5 步永久生效。将来若要给格式内交叉校验设阈值或自动化，须**另开新票**重走 §3.2 五步，并在该票里一并处理上述口径限制。

## 5. 零改动声明

本文只新增一份 `docs/research/` 文档并回填规格 §8.3 状态表：**不改生产代码、不改任何测试用例 / 断言 / `MESSAGE` 文本、不注册 `add_test`、不补素材、不改 `P`、不改任何既有判据数值**。14 个 `sdk_*` 目标、7 组 `TIMEOUT`、15 行覆盖度矩阵、BadInput 55 例 / 396 断言、O5 归档的每 run 24 行 `peakHeldBytes` 基线全部一字不动。
