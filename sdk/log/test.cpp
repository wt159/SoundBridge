#include "LogWrapper.h"
#include <unistd.h>

int main(int argc, char** argv)
{
    printf("LogWrapper::getInstanceInitialize\n");
    std::string rotateFileLog = "logrotate_file_test";
    std::string directory = "./log";
    constexpr int k10KBInBytes = 10 * 1024;
    constexpr int k20InCounts = 20;
    printf("LogWrapper::getInstanceInitialize\n");
    LogWrapper::getInstanceInitialize(directory, rotateFileLog, k10KBInBytes, k20InCounts);
    printf("LogWrapper::getInstanceInitialize done\n");
    size_t num = 0;
    while (1) {
        LOG_INFO("test", "test log num is %ld", num);
        LOG_DEBUG("test", "test log num is %ld", num);
        LOG_WARNING("test", "test log num is %ld", num);
        usleep(2000);
        num++;
        if(num >= 10000) {
            LOG_FATAL("test", "test log num is %ld", num);
            break;
        }
    }
}
