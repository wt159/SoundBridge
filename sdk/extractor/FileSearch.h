#pragma once
#include "ExtractorHelper.hpp"
#include <string>
#include <vector>

std::vector<std::string> recursiveFileSearch(const std::string &rootDir,
    std::unordered_map<std::string, standardExtractors> extractorMap = defaultExtractorMap);
