#pragma once

// 并行测试（ctest -j）下的进程隔离工具。
//
// ctest -j 会同时起多个测试进程，所有用「固定名字」落到共享位置的东西都会互相打架。
// 实测：`ctest -j4` 下四个 UnitTests 进程共用 `./log/unittest_<秒级时间戳>.log`，
// 同一秒启动就撞同一个文件名，先打开的那个轮转/删除时另一个 `open` 直接 EIO
// （`Failed to open file for writing: Input/output error`）。
// 约定：凡是测试进程写出去的路径，都必须带上 processId()。

#include <string>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace sb_test {

inline int processId()
{
#ifdef _WIN32
    return static_cast<int>(_getpid());
#else
    return static_cast<int>(getpid());
#endif
}

// 建目录，已存在不算错。g3log 的 LogRotate 内部是 exists() + create_directories()
// 两步，并发时中间被别的进程建好就会抛异常，所以调用方先建一次。
inline void ensureDirectory(const std::string &path)
{
#ifdef _WIN32
    _mkdir(path.c_str());
#else
    mkdir(path.c_str(), 0755);
#endif
}

} // namespace sb_test
