#include "FileSearch.h"
#include "LogWrapper.h"
#include <boost/filesystem.hpp>
#include <iostream>

#define LOG_TAG "FileSearch"

namespace fs = boost::filesystem;

std::vector<std::string> recursiveFileSearch(
    const std::string &rootDir, std::unordered_map<std::string, standardExtractors> extractorMap)
{
    std::vector<std::string> fileVec;
    fs::path rootPath(rootDir);

    if (!fs::exists(rootPath) || !fs::is_directory(rootPath)) {
        LOG_FATAL(LOG_TAG, "Invalid directory: %s", rootDir.c_str());
        return fileVec;
    }

    fs::recursive_directory_iterator iter(rootPath);
    fs::recursive_directory_iterator end;

    while (iter != end) {
        std::string extensionName = iter->path().extension().string();
        if (fs::is_regular_file(*iter) && extractorMap.find(extensionName) != extractorMap.end()) {
            fileVec.push_back(iter->path().generic_string());
            LOG_DEBUG(LOG_TAG, "file name:%s", iter->path().filename().string().c_str());
        }

        boost::system::error_code ec;
        iter.increment(ec);

        if (ec) {
            LOG_ERROR(LOG_TAG, "Error accessing file: %s", ec.message().c_str());
        }
    }
    return fileVec;
}
