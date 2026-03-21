#pragma once

#include <string>

namespace sdk {

enum class SdkLogLevel {
    Info,
    Warning,
    Error,
    Fatal,
    Debug,
    Verbose
};

struct SdkLogConfig {
    std::string directory = "./log";
    std::string filePrefix = "soundbridge";
    int singleFileSizeBytes = 10 * 1024 * 1024;
    int maxFileCount = 20;
};

bool InitializeLogging(const SdkLogConfig &config);
bool IsLoggingInitialized();
void LogMessage(SdkLogLevel level, const char *tag, const std::string &message);
void LogPrintf(SdkLogLevel level, const char *tag, const char *format, ...);
void LogPrintfWithTrace(SdkLogLevel level, const char *tag, const std::string &traceId, const char *format, ...);

} // namespace sdk
