#pragma once

#include <cstdint>
#include <string>

namespace sdk {

struct MetricTags {
    std::string traceId;
    std::string file;
    int index = -1;

    MetricTags() = default;
    MetricTags(const std::string &trace, const std::string &filePath, int idx)
        : traceId(trace)
        , file(filePath)
        , index(idx)
    {
    }
};

class Metrics {
public:
    static void RecordTiming(const char *name, uint64_t ms, const MetricTags &tags);
    static void RecordCount(const char *name, int64_t value, const MetricTags &tags);
    static void RecordGauge(const char *name, double value, const MetricTags &tags, const char *unit);
};

} // namespace sdk
