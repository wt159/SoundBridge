#pragma once
#include <g3log/g3log.hpp>
#include <g3log/logworker.hpp>
#include <g3sinks/LogRotate.h>
#include <string>
#include <NonCopyable.hpp>

class LogWrapper : public NonCopyable
{
private:
    std::string m_logDir;
    std::string m_logFileName;
    int m_singleFileSizeInBytes;
    int m_maxFileCount;
    bool isRotateLogShutDown;
    std::unique_ptr<g3::LogWorker> m_logWorker;
    std::unique_ptr<g3::SinkHandle<LogRotate>> m_rotateSinkHandle;
    static LogWrapper *m_pInstance;
public:
    static void getInstanceInitialize(std::string& logDir, std::string& logFileName, int singleFileSizeInBytes, int maxFileCount);
public:
    LogWrapper() = delete;
    ~LogWrapper();
private:
    LogWrapper(std::string& logDir, std::string& logFileName, int singleFileSizeInBytes, int maxFileCount);
    void init();
};

#define LOG_INFO(tag, ...) LOGF(INFO, tag": " __VA_ARGS__)
#define LOG_DEBUG(tag, ...) LOGF(DEBUG, tag": " __VA_ARGS__)
#define LOG_WARNING(tag, ...) LOGF(WARNING, tag": " __VA_ARGS__)
#define LOG_ERROR(tag, ...) LOGF(WARNING, tag": " __VA_ARGS__)
#define LOG_FATAL(tag, ...) LOGF(FATAL, tag": " __VA_ARGS__)