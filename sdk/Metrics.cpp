#include "Metrics.h"

#include "LogApi.h"

#include <sstream>

namespace sdk {
namespace {

    std::string SanitizeTagValue(const std::string &value)
    {
        std::string out;
        out.reserve(value.size());
        for (char ch : value) {
            if (ch == '\n' || ch == '\r') {
                continue;
            }
            out.push_back(ch);
        }
        return out;
    }

    void LogMetric(const char *type, const char *name, const std::string &value,
                   const MetricTags &tags, const char *unit)
    {
        std::ostringstream oss;
        oss << "metric=" << (name ? name : "unknown") << " type=" << (type ? type : "unknown")
            << " value=" << value;
        if (unit && unit[0] != '\0') {
            oss << " unit=" << unit;
        }
        if (!tags.traceId.empty()) {
            oss << " traceId=" << tags.traceId;
        }
        if (tags.index >= 0) {
            oss << " index=" << tags.index;
        }
        if (!tags.file.empty()) {
            oss << " file=" << SanitizeTagValue(tags.file);
        }
        LogMessage(SdkLogLevel::Info, "Metrics", oss.str());
    }

} // namespace

void Metrics::RecordTiming(const char *name, uint64_t ms, const MetricTags &tags)
{
    LogMetric("timing", name, std::to_string(ms), tags, "ms");
}

void Metrics::RecordCount(const char *name, int64_t value, const MetricTags &tags)
{
    LogMetric("count", name, std::to_string(value), tags, "count");
}

void Metrics::RecordGauge(const char *name, double value, const MetricTags &tags, const char *unit)
{
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(3);
    oss << value;
    LogMetric("gauge", name, oss.str(), tags, unit);
}

} // namespace sdk
