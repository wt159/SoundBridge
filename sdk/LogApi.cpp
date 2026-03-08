#include "LogApi.h"

#include "log/boost_log/LogWrapper.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace sdk {
namespace {

constexpr int kDefaultSingleFileSizeBytes = 10 * 1024 * 1024;
constexpr int kDefaultMaxFileCount = 20;

std::mutex g_logInitMutex;

int ToWrapperLevel(SdkLogLevel level)
{
    switch (level) {
    case SdkLogLevel::Info:
        return INFO;
    case SdkLogLevel::Warning:
        return WARNING;
    case SdkLogLevel::Error:
        return ERROR;
    case SdkLogLevel::Fatal:
        return FATAL;
    case SdkLogLevel::Debug:
        return DEBUG;
    case SdkLogLevel::Verbose:
        return VERBOSE;
    }
    return INFO;
}

void EnsureDefaultLoggerInitialized()
{
    if (LogWrapper::getInstance() != nullptr) {
        return;
    }

    std::string directory = "./log";
    std::string filePrefix = "soundbridge";
    LogWrapper::getInstanceInitialize(directory, filePrefix, kDefaultSingleFileSizeBytes, kDefaultMaxFileCount);
}

} // namespace

bool InitializeLogging(const SdkLogConfig &config)
{
    std::lock_guard<std::mutex> lock(g_logInitMutex);
    if (LogWrapper::getInstance() != nullptr) {
        return true;
    }

    std::string directory = config.directory.empty() ? "./log" : config.directory;
    std::string filePrefix = config.filePrefix.empty() ? "soundbridge" : config.filePrefix;
    int singleFileSizeBytes = config.singleFileSizeBytes > 0 ? config.singleFileSizeBytes : kDefaultSingleFileSizeBytes;
    int maxFileCount = config.maxFileCount > 0 ? config.maxFileCount : kDefaultMaxFileCount;

    LogWrapper::getInstanceInitialize(directory, filePrefix, singleFileSizeBytes, maxFileCount);
    return LogWrapper::getInstance() != nullptr;
}

bool IsLoggingInitialized()
{
    return LogWrapper::getInstance() != nullptr;
}

void LogMessage(SdkLogLevel level, const char *tag, const std::string &message)
{
    std::lock_guard<std::mutex> lock(g_logInitMutex);
    EnsureDefaultLoggerInitialized();
    if (LogWrapper::getInstance() == nullptr) {
        return;
    }

    const char *safeTag = (tag == nullptr || tag[0] == '\0') ? "SDK" : tag;
    LogWrapper::getInstance()->log(ToWrapperLevel(level), safeTag, "%s", message.c_str());
}

void LogPrintf(SdkLogLevel level, const char *tag, const char *format, ...)
{
    char buffer[2048] = {0};

    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    LogMessage(level, tag, buffer);
}

} // namespace sdk
