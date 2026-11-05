#pragma once

#include <cstddef>

// 解码侧的供给观察点（票面 #60 / 规格第 5 节的「分配记账峰值」与「并行」两条判据）。
//
// 增量供给下「为当前曲目持有多少音频数据」会先增后减：解码线程每拿到一包就 +bytes、
// 每放掉一包就 -bytes。只在 start / 首帧 / EOS 三处采样会把峰值低估，所以记账必须挂在
// alloc / free 这两个事件上（onPacketAcquired / onPacketReleased）。
//
// 「并行」用它数出的包数做结构证据（不是计时）：首帧交付时拉到的包数严格小于序列耗尽时的
// 包数，说明首帧不是「整段解完 / 整段装载」之后才有的；整块形式下两个数相等，必红。
class DecodeSupplyObserver {
public:
    virtual ~DecodeSupplyObserver() { }

    // 解码侧拿走一包（已计入常驻）。在包被丢弃时必须配对调用 onPacketReleased。
    virtual void onPacketAcquired(size_t bytes) = 0;
    virtual void onPacketReleased(size_t bytes) = 0;
    // 本任务累计已从包工厂拉到的包数（含被跳过的空包之外的所有产出）。
    virtual void onPacketPulled(size_t totalPulled) = 0;
};
