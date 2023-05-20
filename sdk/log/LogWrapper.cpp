#include "LogWrapper.h"
#include <g3log/loglevels.hpp>
#include <g3log/logworker.hpp>
#include <g3log/std2_make_unique.hpp>
#include <g3sinks/LogRotate.h>
#include <g3sinks/LogRotateWithFilter.h>
#include <iostream>
#include <unistd.h>

using namespace g3;

LogWrapper* LogWrapper::m_pInstance = nullptr;

void LogWrapper::getInstanceInitialize(std::string& logDir, std::string& logFileName, int singleFileSizeInBytes, int maxFileCount)
{
    if (LogWrapper::m_pInstance == nullptr) {
        LogWrapper::m_pInstance = new LogWrapper(logDir, logFileName, singleFileSizeInBytes, maxFileCount);
    }
}

LogWrapper::LogWrapper(std::string& logDir, std::string& logFileName, int singleFileSizeInBytes, int maxFileCount)
    : m_logDir(logDir)
    , m_logFileName(logFileName)
    , m_singleFileSizeInBytes(singleFileSizeInBytes)
    , m_maxFileCount(maxFileCount)
    , isRotateLogShutDown(false)
    , m_logWorker(LogWorker::createLogWorker())
    , m_rotateSinkHandle(nullptr)
{
    init();
}

LogWrapper::~LogWrapper()
{
    if(isRotateLogShutDown) {
        m_rotateSinkHandle->call(&LogRotate::rotateLog);
    }
    g3::internal::shutDownLogging();
}

void LogWrapper::init()
{
    g3::initializeLogging(m_logWorker.get());
    m_rotateSinkHandle = m_logWorker->addSink(std2::make_unique<LogRotate>(m_logFileName, m_logDir),
        &LogRotate::save);
    m_rotateSinkHandle->call(&LogRotate::setMaxLogSize, m_singleFileSizeInBytes);
    m_rotateSinkHandle->call(&LogRotate::setMaxArchiveLogCount, m_maxFileCount);
}