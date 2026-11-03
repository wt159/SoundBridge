#include "../fixtures/RealMediaFixture.hpp"
#include <AudioCommon.hpp>
#include <ExtractorFactory.h>
#include <ExtractorHelper.hpp>
#include <FileSource.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <doctest/doctest.h>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace {
// 坏输入素材一律字节手术合成到系统临时目录，不入库；析构时删干净（规格 10.#25）。
// 读源走 FileSource，和生产线同一条路径解析（中文名素材在 Windows 上只能这样读）。
class TempFileSet {
public:
    explicit TempFileSet(const char *tag)
        : m_tag(tag)
    {
        const char *base = std::getenv("TEMP");
        if (base == nullptr || base[0] == '\0') {
            base = std::getenv("TMPDIR");
        }
        m_dir = (base != nullptr && base[0] != '\0') ? std::string(base) : std::string(".");
    }

    ~TempFileSet()
    {
        for (const std::string &path : m_files) {
            std::remove(path.c_str());
        }
        for (const std::string &dir : m_dirs) {
            std::remove(dir.c_str());
        }
    }

    std::string make(const char *extension)
    {
        std::string path = m_dir + "/sb_badinput_" + m_tag + "_" + std::to_string(++s_counter)
            + extension;
        m_files.push_back(path);
        return path;
    }

    std::string makeDir()
    {
        std::string path = m_dir + "/sb_badinput_" + m_tag + "_dir" + std::to_string(++s_counter);
#ifdef _WIN32
        _mkdir(path.c_str());
#else
        mkdir(path.c_str(), 0700);
#endif
        m_dirs.push_back(path);
        return path;
    }

private:
    static int s_counter;
    std::string m_dir;
    std::string m_tag;
    std::vector<std::string> m_files;
    std::vector<std::string> m_dirs;
};

int TempFileSet::s_counter = 0;

std::vector<char> readWholeFile(const std::string &path)
{
    std::vector<char> data;
    FileSource source(path.c_str());
    if (source.initCheck() != sdk_utils::OK) {
        return data;
    }
    off64_t size = 0;
    if (source.getSize(&size) != sdk_utils::OK || size <= 0) {
        return data;
    }
    data.resize(static_cast<size_t>(size));
    ssize_t got = source.readAt(0, data.data(), data.size());
    if (got != static_cast<ssize_t>(data.size())) {
        data.clear();
    }
    return data;
}

bool writeWholeFile(const std::string &path, const std::vector<char> &data)
{
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return out.good();
}

// 把一段区间归零：长度不变，模拟「魔数被毁 / 必需结构被毁」（规格 5.5）。
void zeroRange(std::vector<char> &data, size_t offset, size_t length)
{
    for (size_t i = 0; i < length && offset + i < data.size(); ++i) {
        data[offset + i] = 0;
    }
}

// 只清掉掩码里的位，其余字节保持原样：用来单独破坏一个头字段。
void clearBits(std::vector<char> &data, size_t offset, unsigned char mask)
{
    if (offset < data.size()) {
        data[offset] = static_cast<char>(static_cast<unsigned char>(data[offset]) & ~mask);
    }
}

struct ExtractorOutcome {
    bool created    = false;
    bool initOk     = false;
    int sampleRate  = 0;
    int numChannel  = 0;
    uint64_t durationMs = 0;
};

// 与生产线同一个出口：createExtractor(enableSniff=true) 拿到提取器，再看 initCheck()。
ExtractorOutcome openExtractor(const std::string &path, const char *extension)
{
    ExtractorOutcome outcome;
    FileSource source(path.c_str());
    if (source.initCheck() != sdk_utils::OK) {
        return outcome;
    }
    std::unique_ptr<ExtractorHelper> extractor(
        ExtractorFactory::createExtractor(&source, extension, true));
    if (!extractor) {
        return outcome;
    }
    outcome.created = true;
    outcome.initOk  = extractor->initCheck() == sdk_utils::OK;
    // getAudioSpec() 不在禁止调用的数据接口之列（H5 禁的是 metadata / seek / 数据长度）。
    AudioSpec spec      = extractor->getAudioSpec();
    outcome.sampleRate  = spec.sampleRate;
    outcome.numChannel  = spec.numChannel;
    outcome.durationMs  = spec.durationMs;
    return outcome;
}

// 「拒绝」的判据：工厂自己返回 nullptr，或提取器 init 失败。
// 出口的 spec 校验（规格 10.#14）落地前工厂仍会返回对象，所以两种形态都要认。
bool rejected(const ExtractorOutcome &outcome)
{
    return !outcome.created || !outcome.initOk;
}

long long elapsedMs(std::chrono::steady_clock::time_point begin)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - begin)
        .count();
}

long long fileBytes(const std::string &path)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return 0;
    }
    return static_cast<long long>(in.tellg());
}

// 日志目录的总字节数。坏文件让解析循环原地打转时，日志会以每秒数万行的速度增长，
// 所以「日志有上界」可以用目录增长量来判（规格 10.#3 的日志洪水）。
long long logDirBytes(const std::string &dir)
{
    long long total = 0;
#ifdef _WIN32
    std::string pattern = dir + "/*.log";
    struct _finddata_t entry;
    intptr_t handle = _findfirst(pattern.c_str(), &entry);
    if (handle == -1) {
        return 0;
    }
    do {
        total += fileBytes(dir + "/" + entry.name);
    } while (_findnext(handle, &entry) == 0);
    _findclose(handle);
#else
    DIR *handle = opendir(dir.c_str());
    if (handle == nullptr) {
        return 0;
    }
    while (struct dirent *entry = readdir(handle)) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        total += fileBytes(dir + "/" + entry->d_name);
    }
    closedir(handle);
#endif
    return total;
}

// unit 测试把日志写在 cwd 下的 ./log（见 UnitTestsMain.cpp）。
const char *kLogDir = "./log";
} // namespace

TEST_SUITE("BadInput")
{
    // 规格 10.#1 / 5.5 #4：struct128 抹掉 STREAMINFO 里的 sampleRate，旧代码在算时长时整数除零，
    // 进程直接死（exit 0xC0000094）。红的表现就是整个测试进程消失。
    TEST_CASE("flac streaminfo with zero sample rate is rejected without crash")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("小镇姑娘-陶喆.flac")) {
            return;
        }
        TempFileSet tmp("flac_zero_sr");
        std::vector<char> data = readWholeFile(fixture.mediaPath("小镇姑娘-陶喆.flac"));
        REQUIRE(!data.empty());
        // 只在 [12,140) 做手术：魔数留着，紧跟其后的必需结构被毁
        zeroRange(data, 12, 128);
        std::string badPath = tmp.make(".flac");
        REQUIRE(writeWholeFile(badPath, data));

        std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
        ExtractorOutcome outcome                    = openExtractor(badPath, ".flac");
        long long cost                              = elapsedMs(begin);

        CHECK(cost < 5000);
        CHECK(rejected(outcome));
        // 拒绝时不允许把「sr=0」这种 unusable spec 当成正常结果带出去
        CHECK(outcome.sampleRate == 0);
        CHECK(outcome.durationMs == 0);
    }

    // 规格 10.#2：旧代码用 `uint8_t data[blockSize]`（栈上变长数组）读 STREAMINFO，
    // blockSize 上限 0xFFFFFF = 16MB，超过默认栈就是栈溢出。
    TEST_CASE("flac oversized metadata block does not allocate on the stack")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("小镇姑娘-陶喆.flac")) {
            return;
        }
        TempFileSet tmp("flac_big_block");
        std::vector<char> data = readWholeFile(fixture.mediaPath("小镇姑娘-陶喆.flac"));
        REQUIRE(!data.empty());
        // FLAC 布局：[0,4) "fLaC"，[4,8) 第一个元数据块头（1B 标志位 + 3B 长度），
        // 把长度改写成 0xFFFFFF，块类型仍是 STREAMINFO
        data[5] = static_cast<char>(0xFF);
        data[6] = static_cast<char>(0xFF);
        data[7] = static_cast<char>(0xFF);
        std::string badPath = tmp.make(".flac");
        REQUIRE(writeWholeFile(badPath, data));

        std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
        ExtractorOutcome outcome                    = openExtractor(badPath, ".flac");
        long long cost                              = elapsedMs(begin);

        CHECK(cost < 5000);
        // STREAMINFO 恒为 34 字节，声明 16MB 一定是坏头
        CHECK(rejected(outcome));
    }

    // 规格 10.#3 / 5.5 #5：对象长度为 0 时 offset 原地打转，6 秒能写 5.4 万行日志。
    // 红的表现是这条用例不返回（由 ctest 的 TIMEOUT 兜底），绿的表现是秒级返回。
    TEST_CASE("asf object with zero size terminates instead of spinning")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("萨克斯机.asf")) {
            return;
        }
        TempFileSet tmp("asf_zero_obj");
        std::vector<char> data = readWholeFile(fixture.mediaPath("萨克斯机.asf"));
        REQUIRE(!data.empty());
        zeroRange(data, 12, 128);
        std::string badPath = tmp.make(".asf");
        REQUIRE(writeWholeFile(badPath, data));

        long long logBefore                         = logDirBytes(kLogDir);
        std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
        ExtractorOutcome outcome                    = openExtractor(badPath, ".asf");
        long long cost                              = elapsedMs(begin);
        long long logAfter                          = logDirBytes(kLogDir);

        CHECK(cost < 5000);
        CHECK(rejected(outcome));
        // 一次拒绝最多留几十行日志；1MB 是给并发日志留的余量，仍远小于「每秒 9 万行」
        CHECK(logAfter - logBefore < 1024 * 1024);
    }

    TEST_CASE("wma object with zero size terminates instead of spinning")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("十七岁-陶喆.96.wma")) {
            return;
        }
        TempFileSet tmp("wma_zero_obj");
        std::vector<char> data = readWholeFile(fixture.mediaPath("十七岁-陶喆.96.wma"));
        REQUIRE(!data.empty());
        zeroRange(data, 12, 128);
        std::string badPath = tmp.make(".wma");
        REQUIRE(writeWholeFile(badPath, data));

        long long logBefore                         = logDirBytes(kLogDir);
        std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
        ExtractorOutcome outcome                    = openExtractor(badPath, ".wma");
        long long cost                              = elapsedMs(begin);
        long long logAfter                          = logDirBytes(kLogDir);

        CHECK(cost < 5000);
        CHECK(rejected(outcome));
        CHECK(logAfter - logBefore < 1024 * 1024);
    }

    // 规格 10.#4 / 5.5 #3：旧代码从不校验 syncWord，全零头被解析成 sr=96000 ch=0 且 init 成功，
    // 一份垃圾头就这样进了播放列表。
    TEST_CASE("aac header with zero sync word is rejected")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("WavinFlag.aac")) {
            return;
        }
        TempFileSet tmp("aac_zero_sync");
        std::vector<char> data = readWholeFile(fixture.mediaPath("WavinFlag.aac"));
        REQUIRE(!data.empty());
        zeroRange(data, 0, 12);
        std::string badPath = tmp.make(".aac");
        REQUIRE(writeWholeFile(badPath, data));

        ExtractorOutcome outcome = openExtractor(badPath, ".aac");
        CHECK(rejected(outcome));
    }

    TEST_CASE("aac header with zero channel configuration is rejected")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("WavinFlag.aac")) {
            return;
        }
        TempFileSet tmp("aac_zero_ch");
        std::vector<char> data = readWholeFile(fixture.mediaPath("WavinFlag.aac"));
        REQUIRE(!data.empty());
        // 只清 channelConfiguration 的三个位：data[2] bit0 + data[3] bit7..6。
        // syncWord / samplingFrequencyIndex 原样保留，所以「靠同步字侥幸通过」的实现在这里仍然会红。
        clearBits(data, 2, 0x01);
        clearBits(data, 3, 0xC0);
        std::string badPath = tmp.make(".aac");
        REQUIRE(writeWholeFile(badPath, data));

        ExtractorOutcome outcome = openExtractor(badPath, ".aac");
        CHECK(rejected(outcome));
    }
}
