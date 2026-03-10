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
    using ExtractorSniffer = std::function<bool(DataSourceBase *)>;

    ExtractorFactory() = delete;
    ~ExtractorFactory() = delete;

    // 娉ㄥ唽鎵╁睍鍚嶅埌鎻愬彇鍣ㄦ瀯閫犲嚱鏁般€?
    // 杩斿洖 false 琛ㄧず璇ユ墿灞曞悕宸叉敞鍐岋紙涓嶄細瑕嗙洊鏃у疄鐜帮級銆?
    static bool registerExtractor(const std::string &extensionName, ExtractorCreator creator);
    static bool registerExtractor(const std::string &extensionName, ExtractorCreator creator, ExtractorSniffer sniffer);

    static ExtractorHelper *createExtractor(DataSourceBase *source, const std::string &extensionName);
    static ExtractorHelper *createExtractor(DataSourceBase *source,
                                            const std::string &extensionName,
                                            bool enableSniff);
};

// 绠€鍖栨敞鍐屽啓娉曪細鍦?cpp 鏂囦欢涓竴琛屽畬鎴愨€滄墿灞曞悕 -> 鎻愬彇鍣ㄧ被鍨嬧€濈殑闈欐€佹敞鍐屻€?
#define SB_EXTRACTOR_CONCAT_IMPL(A, B) A##B
#define SB_EXTRACTOR_CONCAT(A, B) SB_EXTRACTOR_CONCAT_IMPL(A, B)

#define SB_REGISTER_EXTRACTOR_IMPL(ID, EXTENSION, TYPE)                                      \
    namespace {                                                                                \
    const bool SB_EXTRACTOR_CONCAT(g_registered_extractor_, ID) =                             \
        ExtractorFactory::registerExtractor(                                                   \
        EXTENSION,                                                                             \
        [](DataSourceBase *source) -> ExtractorHelper * { return new TYPE(source); });        \
    }

#define REGISTER_EXTRACTOR(EXTENSION, TYPE) SB_REGISTER_EXTRACTOR_IMPL(__COUNTER__, EXTENSION, TYPE)

#define SB_REGISTER_EXTRACTOR_WITH_SNIFF_IMPL(ID, EXTENSION, TYPE)                           \
    namespace {                                                                               \
    const bool SB_EXTRACTOR_CONCAT(g_registered_extractor_, ID) =                            \
        ExtractorFactory::registerExtractor(                                                  \
        EXTENSION,                                                                            \
        [](DataSourceBase *source) -> ExtractorHelper * { return new TYPE(source); },        \
        [](DataSourceBase *source) -> bool { return TYPE::sniff(source); });                 \
    }

#define REGISTER_EXTRACTOR_WITH_SNIFF(EXTENSION, TYPE)                                        \
    SB_REGISTER_EXTRACTOR_WITH_SNIFF_IMPL(__COUNTER__, EXTENSION, TYPE)










