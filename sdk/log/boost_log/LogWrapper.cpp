#include "LogWrapper.h"

#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/log/attributes.hpp>
#include <boost/log/common.hpp>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_file_backend.hpp>
#include <boost/log/sources/severity_logger.hpp>
#include <boost/log/support/date_time.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/make_shared.hpp>

#include <cstdarg>
#include <cstring>

namespace logging  = boost::log;
namespace attrs    = boost::log::attributes;
namespace src      = boost::log::sources;
namespace sinks    = boost::log::sinks;
namespace expr     = boost::log::expressions;
namespace keywords = boost::log::keywords;

enum severity_level { normal, warning, error, fatal, debug, verbose };

template <typename CharT, typename TraitsT>
inline std::basic_ostream<CharT, TraitsT> &operator<<(std::basic_ostream<CharT, TraitsT> &strm,
                                                      severity_level lvl)
{
    static const char *const str[] = { "I", "W", "E", "F", "D", "V" };
    if (static_cast<std::size_t>(lvl) < (sizeof(str) / sizeof(*str))) {
        strm << str[lvl];
    } else {
        strm << static_cast<int>(lvl);
    }
    return strm;
}

std::unique_ptr<LogWrapper> LogWrapper::m_staticLog;

class LogWrapper::Impl {
private:
    using file_sink = sinks::synchronous_sink<sinks::text_file_backend>;

public:
    Impl(std::string &logDir, std::string &logFileName, int singleFileSizeInBytes, int maxFileCount)
        : m_sink(nullptr)
        , m_slg()
        , m_logDir(logDir)
        , m_logFileName(logFileName)
        , m_singleFileSizeInBytes(singleFileSizeInBytes)
        , m_maxFileCount(maxFileCount)
    {
        init();
    }

    ~Impl()
    {
        if (m_sink) {
            m_sink->flush();
        }
    }

    void write(int level, const char *tag, const char *format, va_list args)
    {
        char buf[1024]      = { 0 };
        const char *safeTag = (tag == nullptr || tag[0] == '\0') ? "SDK" : tag;
        const int written   = snprintf(buf, sizeof(buf), "[%s] ", safeTag);
        const int prefixLen = (written > 0 && written < static_cast<int>(sizeof(buf)))
            ? written
            : static_cast<int>(sizeof(buf)) - 1;
        vsnprintf(buf + prefixLen, sizeof(buf) - prefixLen, format, args);

        switch (level) {
        case logLevel::INFO:
            BOOST_LOG_SEV(m_slg, normal) << buf;
            break;
        case logLevel::ERROR:
            BOOST_LOG_SEV(m_slg, error) << buf;
            break;
        case logLevel::WARNING:
            BOOST_LOG_SEV(m_slg, warning) << buf;
            break;
        case logLevel::FATAL:
            BOOST_LOG_SEV(m_slg, fatal) << buf;
            break;
        case logLevel::DEBUG:
            BOOST_LOG_SEV(m_slg, debug) << buf;
            break;
        case logLevel::VERBOSE:
            BOOST_LOG_SEV(m_slg, verbose) << buf;
            break;
        default:
            break;
        }
    }

private:
    void init()
    {
        std::string fileNamePattern = m_logDir + "/" + m_logFileName + "_%Y%m%d_%H%M%S_%5N.log";
        std::string targetPattern   = m_logFileName + "_%Y%m%d_%H%M%S_%5N.log";

        auto sink
            = boost::make_shared<file_sink>(keywords::file_name        = fileNamePattern.c_str(),
                                            keywords::target_file_name = targetPattern.c_str(),
                                            keywords::rotation_size    = m_singleFileSizeInBytes);
        sink->locked_backend()->auto_flush(true);

        sink->locked_backend()->set_file_collector(sinks::file::make_collector(
            keywords::target   = m_logDir,
            keywords::max_size = static_cast<uintmax_t>(m_singleFileSizeInBytes)
                * static_cast<uintmax_t>(m_maxFileCount),
            keywords::max_files = static_cast<uintmax_t>(m_maxFileCount)));

        sink->locked_backend()->scan_for_files();

        sink->set_formatter(expr::stream
                            << "["
                            << expr::format_date_time<boost::posix_time::ptime>(
                                   "TimeStamp", "%Y-%m-%d %H:%M:%S.%f")
                            << "] [" << expr::attr<attrs::current_thread_id::value_type>("ThreadID")
                            << "] [" << expr::attr<severity_level>("Severity") << "] - "
                            << expr::smessage);

        logging::core::get()->add_sink(sink);
        logging::add_common_attributes();

        m_sink = sink;
    }

private:
    boost::shared_ptr<file_sink> m_sink;
    src::severity_logger<severity_level> m_slg;
    std::string m_logDir;
    std::string m_logFileName;
    int m_singleFileSizeInBytes;
    int m_maxFileCount;
};

void LogWrapper::initialize(std::string &logDir, std::string &logFileName,
                            int singleFileSizeInBytes, int maxFileCount)
{
    if (m_staticLog) {
        return;
    }
    m_staticLog.reset(new LogWrapper());
    m_staticLog->m_impl.reset(
        new LogWrapper::Impl(logDir, logFileName, singleFileSizeInBytes, maxFileCount));
}

void LogWrapper::getInstanceInitialize(std::string &logDir, std::string &logFileName,
                                       int singleFileSizeInBytes, int maxFileCount)
{
    LogWrapper::initialize(logDir, logFileName, singleFileSizeInBytes, maxFileCount);
}

LogWrapper *LogWrapper::getInstance()
{
    return m_staticLog.get();
}

void LogWrapper::log(int level, const char *tag, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    m_impl->write(level, tag, format, args);
    va_end(args);
}
