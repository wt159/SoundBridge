#pragma once

#include <boost/filesystem.hpp>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace sdk_utils {

// Convert boost::filesystem::path to UTF-8 std::string
inline std::string toUtf8String(const boost::filesystem::path &p)
{
#ifdef _WIN32
    std::wstring ws = p.wstring();
    if (ws.empty())
        return std::string();
    int size = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return std::string();
    std::string utf8(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &utf8[0], size, nullptr, nullptr);
    return utf8;
#else
    return p.string();
#endif
}

// Convert UTF-8 string to native encoding for std::ifstream / fopen
// Windows: UTF-8 → ANSI (local codepage), Linux: pass-through
inline std::string toNativeString(const char *utf8)
{
#ifdef _WIN32
    if (!utf8 || !utf8[0])
        return std::string();
    // UTF-8 → wstring
    int wsize = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (wsize <= 0)
        return std::string();
    std::wstring ws(static_cast<size_t>(wsize - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &ws[0], wsize);
    // wstring → ANSI
    int asize = WideCharToMultiByte(CP_ACP, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (asize <= 0)
        return std::string();
    std::string ansi(static_cast<size_t>(asize - 1), '\0');
    WideCharToMultiByte(CP_ACP, 0, ws.c_str(), -1, &ansi[0], asize, nullptr, nullptr);
    return ansi;
#else
    return utf8 ? std::string(utf8) : std::string();
#endif
}

} // namespace sdk_utils
