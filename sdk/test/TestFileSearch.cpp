#include "FileSearch.h"
#include "LogWrapper.h"
#include <unordered_map>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <list>
#include <map>
#include <string>
#include <vector>

using namespace std;
#define LOG_TAG "FileSearchTest"

void TestCode()
{
    std::string rootDir = "./../../../music";
    std::unordered_map<std::string, std::string> nameAndPathMap = recursiveFileSearch(rootDir);
    LOG_INFO(LOG_TAG, "nameAndPathMap size:%d", nameAndPathMap.size());
    for (auto &item : nameAndPathMap) {
        LOG_INFO(LOG_TAG, "name:%s", item.first.c_str());
        std::cout << "path:" << item.second << std::endl;
    }
}

int main(int argc, char const *argv[])
{
    std::string rotateFileLog = "file_search_test";
    std::string directory = "./log";
    constexpr int k10MBInBytes = 10 * 1024 * 1024;
    constexpr int k20InCounts = 20;
    printf("LogWrapper::getInstanceInitialize\n");
    LogWrapper::getInstanceInitialize(directory, rotateFileLog, k10MBInBytes, k20InCounts);
    LOG_INFO(LOG_TAG, "Log init success");

    TestCode();

    std::cout << "test end\n";
    return 0;
}
