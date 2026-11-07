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

#ifdef _WIN32
// Convert UTF-8 string to the Windows native wide string.
// 打开文件应该走这条路径：宽字符与系统代码页无关，中文 / 日文 / emoji 之类的名字
// 在任何语言的 Windows 上都能命中（见 FileSource 的 _wfopen）。
inline std::wstring toWideString(const char *utf8)
{
    if (!utf8 || !utf8[0])
        return std::wstring();
    int wsize = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (wsize <= 0)
        return std::wstring();
    std::wstring ws(static_cast<size_t>(wsize - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &ws[0], wsize);
    return ws;
}
#endif

// Convert UTF-8 string to native encoding for narrow-char APIs (std::ifstream / fopen)
// Windows: UTF-8 → ANSI (local codepage), Linux: pass-through
//
// 注意：Windows 上这一步是 UTF-8 → ANSI(CP_ACP)，**有损**。CP_ACP 随系统区域设置变化，
// 中文系统(936)能表示中文而英文系统(1252)不能，映射不上的字符会被替换成 '?'，
// 文件名随之失效。2026-11-07 Windows CI 就是因为这里在 en-US runner 上把中文名素材
// 打成 "?????.ogg" 而整片测试变红。**不要再用它打开文件**，改用 toWideString + _wfopen。
inline std::string toNativeString(const char *utf8)
{
#ifdef _WIN32
    if (!utf8 || !utf8[0])
        return std::string();
    // UTF-8 → wstring
    const std::wstring ws = toWideString(utf8);
    if (ws.empty())
        return std::string();
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
