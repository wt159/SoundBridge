#pragma once

#include "DataSource.hpp"

// P1：同一取消信号的两个只读检查点。注入源是
// AudioStreamDecoder::m_abortDecode（唯一取消信号），本模块不新建信号、
// 不缓存状态、不写任何东西；只读 DataSourceBase 的虚接缝。
inline bool sbAbortRequested(const DataSourceBase *source)
{
    return source != nullptr && source->abortRequested();
}

// AVFormatContext::interrupt_callback 的回调：返回 1 表示请求中断，
// FFmpeg 4.4 会据此返回 AVERROR_EXIT（libavutil/error.h:56、libavformat/avio.h:49）。
inline int sbAbortInterruptCallback(void *opaque)
{
    return sbAbortRequested(static_cast<const DataSourceBase *>(opaque)) ? 1 : 0;
}
