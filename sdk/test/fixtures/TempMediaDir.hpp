#pragma once
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

/**
 * @brief 临时素材目录：在系统临时目录里准备文件，析构时删干净。
 * @details 目录名带进程号（`sb_interaction_<pid>_<tag>`），保证 `ctest -j4` 下各进程
 *          只写自己的路径；路径统一用 '/'，Windows 的 `_mkdir` / `_rmdir` 同样接受。
 */
class TempMediaDir {
public:
    explicit TempMediaDir(const char *tag)
    {
        const char *base = std::getenv("TEMP");
        if (base == nullptr || base[0] == '\0')
            base = std::getenv("TMPDIR");
        const std::string root
            = (base != nullptr && base[0] != '\0') ? std::string(base) : std::string(".");
#ifdef _WIN32
        const int pid = static_cast<int>(_getpid());
#else
        const int pid = static_cast<int>(getpid());
#endif
        m_path = root + "/sb_interaction_" + std::to_string(pid) + "_" + tag;
#ifdef _WIN32
        _mkdir(m_path.c_str());
#else
        mkdir(m_path.c_str(), 0777);
#endif
    }

    ~TempMediaDir()
    {
        for (size_t i = 0; i < m_files.size(); ++i)
            std::remove(m_files[i].c_str());
#ifdef _WIN32
        _rmdir(m_path.c_str());
#else
        rmdir(m_path.c_str());
#endif
    }

    const std::string &dir() const { return m_path; }

    std::string file(const std::string &name)
    {
        const std::string path = m_path + "/" + name;
        m_files.push_back(path);
        return path;
    }

private:
    std::string m_path;
    std::vector<std::string> m_files;
};
