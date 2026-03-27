#include "LogApi.h"
#include "soundbridge/logging.h"
#include <cstdarg>

namespace soundbridge {

bool InitializeLogging(const LogConfig &config)
{
    sdk::SdkLogConfig legacy;
    legacy.directory           = config.directory;
    legacy.filePrefix          = config.filePrefix;
    legacy.singleFileSizeBytes = config.singleFileSizeBytes;
    legacy.maxFileCount        = config.maxFileCount;
    return sdk::InitializeLogging(legacy);
}

bool IsLoggingInitialized()
{
    return sdk::IsLoggingInitialized();
}

void LogMessage(LogLevel level, const char *tag, const std::string &message)
{
    sdk::LogMessage(static_cast<sdk::SdkLogLevel>(level), tag, message);
}

void LogPrintf(LogLevel level, const char *tag, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    char buffer[4096];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    sdk::LogMessage(static_cast<sdk::SdkLogLevel>(level), tag, buffer);
}

void LogPrintfWithTrace(LogLevel level, const char *tag, const std::string &traceId,
                        const char *format, ...)
{
    va_list args;
    va_start(args, format);
    char buffer[4096];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    sdk::LogPrintfWithTrace(static_cast<sdk::SdkLogLevel>(level), tag, traceId, "%s", buffer);
}

} // namespace soundbridge
