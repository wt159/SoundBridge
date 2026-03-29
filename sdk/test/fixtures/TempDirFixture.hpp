#pragma once
#include <filesystem>
#include <string>

class TempDirFixture {
public:
    TempDirFixture()
    {
        std::error_code ec;
        m_path = std::filesystem::temp_directory_path(ec) / "sb_test_XXXXXX";
        m_path = std::filesystem::create_directories(m_path, ec);
    }

    ~TempDirFixture()
    {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }

    std::filesystem::path path() const { return m_path; }

    std::filesystem::path file(const std::string &name) const { return m_path / name; }

private:
    std::filesystem::path m_path;
};
