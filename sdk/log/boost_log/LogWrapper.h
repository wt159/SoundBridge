#pragma once
#include <NonCopyable.hpp>
#include <memory>
#include <string>

#define CHECK(x)       assert(x)

enum logLevel
{
    INFO,
    WARNING,
    ERROR,
    FATAL,
    DEBUG,
    VERBOSE
};

class LogWrapper : NonCopyable {
private:
    class Impl;
    std::shared_ptr<Impl> m_impl;
    static std::unique_ptr<LogWrapper> m_staticLog;

public:
    static void initialize(std::string &logDir, std::string &logFileName,
                                      int singleFileSizeInBytes, int maxFileCount);
    static void getInstanceInitialize(std::string &logDir, std::string &logFileName,
                                      int singleFileSizeInBytes, int maxFileCount);
    static LogWrapper* getInstance();
    void log(int level, const char *tag, const char* format, ...);

private:
    LogWrapper() {}
public:
    ~LogWrapper() = default;
};

#define LOG_INFO(tag, ...)    LogWrapper::getInstance()->log(INFO, tag, __VA_ARGS__)
#define LOG_DEBUG(tag, ...)   LogWrapper::getInstance()->log(DEBUG, tag, __VA_ARGS__)
#define LOG_WARNING(tag, ...) LogWrapper::getInstance()->log(WARNING, tag, __VA_ARGS__)
#define LOG_ERROR(tag, ...)   LogWrapper::getInstance()->log(ERROR, tag, __VA_ARGS__)
#define LOG_FATAL(tag, ...)   LogWrapper::getInstance()->log(FATAL, tag, __VA_ARGS__)
#define LOG_VERBOSE(tag, ...)   LogWrapper::getInstance()->log(VERBOSE, tag, __VA_ARGS__)

#define LOGI(...)             LOG_INFO(LOG_TAG, __VA_ARGS__)
#define LOGE(...)             LOG_ERROR(LOG_TAG, __VA_ARGS__)
#define LOGW(...)             LOG_WARNING(LOG_TAG, __VA_ARGS__)
#define LOGD(...)             LOG_DEBUG(LOG_TAG, __VA_ARGS__)
#define LOGV(...)             LOG_VERBOSE(LOG_TAG, __VA_ARGS__)
