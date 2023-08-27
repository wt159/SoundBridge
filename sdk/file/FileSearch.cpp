#include "FileSearch.h"
#include "LogWrapper.h"
#include <boost/filesystem.hpp>
#include <iostream>

#define LOG_TAG "FileSearch"

namespace fs = boost::filesystem;

std::unordered_map<std::string, std::string> recursiveFileSearch(
    const std::string &rootDir, std::vector<std::string> &incFileExtensionVec)
{
    std::unordered_map<std::string, std::string> nameAndPathMap;
    fs::path rootPath(rootDir);
    std::unordered_map<std::string, bool> incFileExtensionMap;
    for (auto &incFileExtension : incFileExtensionVec) {
        incFileExtensionMap[incFileExtension] = true;
    }

    if (!fs::exists(rootPath) || !fs::is_directory(rootPath)) {
        LOG_FATAL(LOG_TAG, "Invalid directory: %s", rootDir.c_str());
        return nameAndPathMap;
    }

    fs::recursive_directory_iterator iter(rootPath);
    fs::recursive_directory_iterator end;

    while (iter != end) {
        if (fs::is_regular_file(*iter) && incFileExtensionMap[iter->path().extension().string()]) {
            nameAndPathMap[iter->path().filename().string()] = iter->path().string();
            LOG_DEBUG(LOG_TAG, "file name:%s", iter->path().filename().string().c_str());
        }

        boost::system::error_code ec;
        iter.increment(ec);

        if (ec) {
            LOG_ERROR(LOG_TAG, "Error accessing file: %s", ec.message().c_str());
        }
    }
    return nameAndPathMap;
}