#pragma once

#include <cstddef>
#include <cstdint>

// 边解边播的四个观测点（规格 doc/streaming-playback-spec.md 第 5 节，票面 #56 / E5）。
//
// 它们是**仪表，不是判据**：本票只要求这些数值在播放路径上可读、可复现，
// 不对常驻上界、首播延迟或欠流判红绿，也不设任何阈值。判据要等链路切到增量供给
// （#60 起）之后才由同一套观测点给出。
//
// 全部字段都是「本次播放任务」的口径，`AudioStreamDecoder::start()` 清零后重新累积。
struct StreamMetrics {
    // 1) 分配记账峰值：本次播放为当前曲目持有的音频数据字节数。
    //    必须覆盖**一切随包数增长的结构**（payload、包索引、页表、seek 表），
    //    只算 payload 会让常驻上界判据形同虚设（规格第 3 节 / ADR 0003）。
    //    不含量测对象之外的固定开销：ring（262144 B）与 ffmpeg AVIO 缓冲（64 KB）
    //    在判据公式里是独立项，不混进这个数。
    size_t peakHeldBytes = 0;
    // 2) 稳态静音填充次数：消费端在静默窗口里写出的静音块数（次数，不是字节数）。
    //    已知代价（规格第 5 节）：`shouldFillSilence()` 不含 ring 为空的条件，
    //    取不到数据（got == 0）时消费端静默跳过本轮、不填静音，因此这个计数器
    //    只覆盖 shuttingDown / switching / SEEKING 三类窗口。
    //    它判的是「稳态窗口内静音填充为零」，**不是「无欠流」**。
    size_t silenceFills = 0;
    // 3) start() → 首次把 PCM 交给消费端（ring 写入成功）的微秒数。
    //    首帧前的提取器 init 扫描不计入这里，它单独记在 initScanUs。
    uint64_t startToFirstPcmUs = 0;
    // 4) 提取器 init 只读扫描耗时（微秒）。当前链路的 init 仍是「全量装载 payload」，
    //    所以本票记录的是提取器构造到可用的耗时；E2 把 init 改成只读扫描后，
    //    同一个测量点自动变成「只读扫描耗时」。只记录启动开销，不进首播判据、不设阈值。
    uint64_t initScanUs = 0;
    // 5) 本任务从包工厂拉到的包数（累计）。它和下面那条一起构成并行判据的结构证据：
    //    「首帧交付时供给序列尚未耗尽」= 解码与消费并行推进；「首帧时已经拉完」=
    //    先整段解完（或整段装载）才交付。
    size_t packetsPulled = 0;
    // 首帧交付那一刻已拉到的包数。0 表示这次播放根本没走包工厂（旧的全量供给形式）。
    size_t packetsPulledAtFirstPcm = 0;
    // 6) 现场重算包边界的 CPU 成本（微秒，累计）。包边界按 offset 重算的格式（mkv 起）
    //    每次拉包都要 demux 现场解析 cluster / block 头，这份成本只记独立指标、
    //    不设阈值、不参与任何判据（规格第 5 节末段）。其余格式恒为 0。
    uint64_t packetBoundaryMicros = 0;
};
