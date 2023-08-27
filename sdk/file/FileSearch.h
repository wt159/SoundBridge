#pragma once
#include <string>
#include <unordered_map>
#include <vector>

std::vector<std::string> defaultIncFileExtensionVec
    = { ".wav", ".aac", ".mp3", ".m4a", ".asf", ".flac", ".aiff", ".ogg" };

std::unordered_map<std::string, std::string> recursiveFileSearch(const std::string &rootDir,
    std::vector<std::string> &incFileExtensionVec = defaultIncFileExtensionVec);