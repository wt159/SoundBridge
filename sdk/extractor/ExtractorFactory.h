#pragma once
#include "ExtractorHelper.hpp"
#include "DataSource.hpp"
#include "NonCopyable.hpp"
#include <functional>
#include <string>

class ExtractorFactory : public NonCopyable
{
public:
    using ExtractorCreator = std::function<ExtractorHelper *(DataSourceBase *)>;

    ExtractorFactory() = delete;
    ~ExtractorFactory() = delete;

    // 注册扩展名到提取器构造函数。
    // 返回 false 表示该扩展名已注册（不会覆盖旧实现）。
    static bool registerExtractor(const std::string &extensionName, ExtractorCreator creator);

    static ExtractorHelper *createExtractor(DataSourceBase *source, const std::string &extensionName);
};

// 简化注册写法：在 cpp 文件中一行完成“扩展名 -> 提取器类型”的静态注册。
#define SB_REGISTER_EXTRACTOR_IMPL(ID, EXTENSION, TYPE)                                      \
    namespace {                                                                                \
    const bool g_registered_extractor_##ID = ExtractorFactory::registerExtractor(             \
        EXTENSION,                                                                             \
        [](DataSourceBase *source) -> ExtractorHelper * { return new TYPE(source); });        \
    }

#define REGISTER_EXTRACTOR(EXTENSION, TYPE) SB_REGISTER_EXTRACTOR_IMPL(__COUNTER__, EXTENSION, TYPE)
