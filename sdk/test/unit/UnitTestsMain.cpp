#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <LogWrapper.h>
#include <doctest/doctest.h>
#include <string>

struct LogInit {
    LogInit()
    {
        constexpr int k10MBInBytes = 10 * 1024 * 1024;
        constexpr int k20InCounts  = 20;
        std::string logDir         = "./log";
        std::string logFileName    = "unittest";
        LogWrapper::getInstanceInitialize(logDir, logFileName, k10MBInBytes, k20InCounts);
    }
};

static LogInit g_logInit;
