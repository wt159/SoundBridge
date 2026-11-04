#pragma once
#include "DataSource.hpp"
#include "ExtractorHelper.hpp"
#include "NonCopyable.hpp"
#include <functional>
#include <string>
#include <vector>

class ExtractorFactory : public NonCopyable {
public:
    using ExtractorCreator = std::function<ExtractorHelper *(DataSourceBase *)>;
    using ExtractorSniffer = std::function<bool(DataSourceBase *)>;

    ExtractorFactory()  = delete;
    ~ExtractorFactory() = delete;

    // Register an extractor by extension.
    // Returns false if the extension already exists or registration failed.
    //
    // allowEmptySpec 是给测试替身留的逃生口：默认 false 时，出口按 AudioSpec 契约
    // 拒绝 sampleRate/numChannel <= 0 的提取器；探测型替身确实没有真实 spec，
    // 注册时要显式传 true，别让契约被整体放宽。
    static bool registerExtractor(const std::string &extensionName, ExtractorCreator creator);
    static bool registerExtractor(const std::string &extensionName, ExtractorCreator creator,
                                  ExtractorSniffer sniffer, bool allowEmptySpec = false);

    static ExtractorHelper *createExtractor(DataSourceBase *source,
                                            const std::string &extensionName);
    static ExtractorHelper *createExtractor(DataSourceBase *source,
                                            const std::string &extensionName, bool enableSniff);

    // 扩展名是否已注册（大小写、前导点按注册时的规则归一化）。
    // 错误码要按「扩展名是否注册」分档（规格 5.4 H3），调用方靠这个查询口判断，
    // 不要用「createExtractor 返回空再猜」。
    static bool isSupportedExtension(const std::string &extensionName);

    // 当前已注册扩展名的排序快照（含前导点、小写）。
    // 这是「受支持格式」的机器可读真源：doc/format-support.md 是人读版本，
    // 两者由 test_extractor.cpp 的一致性用例锁定，避免文档与注册表各说各话。
    static std::vector<std::string> registeredExtensions();
};

// Helper macros for static registration from the extractor implementation files.
#define SB_EXTRACTOR_CONCAT_IMPL(A, B) A##B
#define SB_EXTRACTOR_CONCAT(A, B)      SB_EXTRACTOR_CONCAT_IMPL(A, B)

#define SB_REGISTER_EXTRACTOR_IMPL(ID, EXTENSION, TYPE)                                            \
    namespace {                                                                                    \
        const bool SB_EXTRACTOR_CONCAT(g_registered_extractor_, ID)                                \
            = ExtractorFactory::registerExtractor(                                                 \
                EXTENSION,                                                                         \
                [](DataSourceBase *source) -> ExtractorHelper * { return new TYPE(source); });     \
    }

#define REGISTER_EXTRACTOR(EXTENSION, TYPE) SB_REGISTER_EXTRACTOR_IMPL(__COUNTER__, EXTENSION, TYPE)

#define SB_REGISTER_EXTRACTOR_WITH_SNIFF_IMPL(ID, EXTENSION, TYPE)                                 \
    namespace {                                                                                    \
        const bool SB_EXTRACTOR_CONCAT(g_registered_extractor_, ID)                                \
            = ExtractorFactory::registerExtractor(                                                 \
                EXTENSION,                                                                         \
                [](DataSourceBase *source) -> ExtractorHelper * { return new TYPE(source); },      \
                [](DataSourceBase *source) -> bool { return TYPE::sniff(source); });               \
    }

#define REGISTER_EXTRACTOR_WITH_SNIFF(EXTENSION, TYPE)                                             \
    SB_REGISTER_EXTRACTOR_WITH_SNIFF_IMPL(__COUNTER__, EXTENSION, TYPE)
