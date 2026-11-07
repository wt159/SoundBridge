#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <LogWrapper.h>
#include <TestProcess.hpp>
#include <doctest/doctest.h>
#include <string>

struct LogInit {
    LogInit()
    {
        constexpr int k10MBInBytes = 10 * 1024 * 1024;
        constexpr int k20InCounts  = 20;
        std::string logDir         = "./log";
        // 文件名带进程号：ctest -j 下多个 UnitTests 进程共用 ./log，
        // 秒级时间戳文件名会在同一秒启动时撞车（见 fixtures/TestProcess.hpp）。
        std::string logFileName = "unittest_" + std::to_string(sb_test::processId());
        sb_test::ensureDirectory(logDir);
        LogWrapper::getInstanceInitialize(logDir, logFileName, k10MBInBytes, k20InCounts);
    }
};

static LogInit g_logInit;
