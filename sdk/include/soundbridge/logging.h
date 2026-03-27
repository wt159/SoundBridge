#pragma once

#include <string>

namespace soundbridge {

enum class LogLevel { Info, Warning, Error, Fatal, Debug, Verbose };

struct LogConfig {
    std::string directory   = "./log";
    std::string filePrefix  = "soundbridge";
    int singleFileSizeBytes = 10 * 1024 * 1024;
    int maxFileCount        = 20;
};

bool InitializeLogging(const LogConfig &config);
bool IsLoggingInitialized();
void LogMessage(LogLevel level, const char *tag, const std::string &message);
void LogPrintf(LogLevel level, const char *tag, const char *format, ...);
void LogPrintfWithTrace(LogLevel level, const char *tag, const std::string &traceId,
                        const char *format, ...);

} // namespace soundbridge
