#include "LogWrapper.h"
#include <stdexcept>
#include <string>
#include <iostream>
#include <thread>
#include <boost/smart_ptr/shared_ptr.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#include <boost/log/common.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/attributes.hpp>
#include <boost/log/sources/logger.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_file_backend.hpp>

namespace logging = boost::log;
namespace attrs = boost::log::attributes;
namespace src = boost::log::sources;
namespace sinks = boost::log::sinks;
namespace expr = boost::log::expressions;
namespace keywords = boost::log::keywords;

using boost::shared_ptr;

enum severity_level
{
    normal,
    warning,
    error,
    fatal,
    debug,
    verbose
};

// The formatting logic for the severity level
template< typename CharT, typename TraitsT >
inline std::basic_ostream< CharT, TraitsT >& operator<< (std::basic_ostream< CharT, TraitsT >& strm, severity_level lvl)
{
    static const char* const str[] =
    {
        "I",
        "W",
        "E",
        "F",
        "D",
        "V"
    };
    if (static_cast< std::size_t >(lvl) < (sizeof(str) / sizeof(*str)))
        strm << str[lvl];
    else
        strm << static_cast< int >(lvl);
    return strm;
}

void thread_func(int i)
{
    src::logger lg;
    BOOST_LOG(lg) << i << " Some log record";
}

class attrs_log_pid:
    public logging::v2_mt_posix::attribute
{
public:
    using value_type = uint32_t;
protected:
    static value_type current_pid() {
        return (value_type)getpid();
    }

protected:
    //! Attribute factory implementation
    struct BOOST_SYMBOL_VISIBLE impl :
        public logging::v2_mt_posix::attribute::impl
    {
        logging::v2_mt_posix::attribute_value get_value()
        {
            typedef logging::v2_mt_posix::attributes::attribute_value_impl< value_type > result_value;
            return logging::v2_mt_posix::attribute_value(new result_value(current_pid()));
        }
    };

public:
    attrs_log_pid() : logging::v2_mt_posix::attribute(new impl()) {}
    ~attrs_log_pid() = default;
};

class attrs_log_tid:
    public logging::v2_mt_posix::attribute
{
public:
    using value_type = uint32_t;
protected:
    static value_type current_tid() {
        return (value_type)syscall(SYS_gettid);
    }

protected:
    //! Attribute factory implementation
    struct BOOST_SYMBOL_VISIBLE impl :
        public logging::v2_mt_posix::attribute::impl
    {
        logging::v2_mt_posix::attribute_value get_value()
        {
            typedef logging::v2_mt_posix::attributes::attribute_value_impl< value_type > result_value;
            return logging::v2_mt_posix::attribute_value(new result_value(current_tid()));
        }
    };

public:
    attrs_log_tid() : logging::v2_mt_posix::attribute(new impl()) {}
    ~attrs_log_tid() = default;
};

std::unique_ptr<LogWrapper> LogWrapper::m_staticLog;

class LogWrapper::Impl
{
private:
    typedef sinks::synchronous_sink< sinks::text_file_backend > file_sink;
    boost::shared_ptr< file_sink > m_sink;
    src::severity_logger< severity_level > m_slg;
    std::string m_logDir;
    std::string m_logFileName;
    int m_singleFileSizeInBytes;
    int m_maxFileCount;
public:
    Impl(std::string & logDir, std::string & logFileName, int singleFileSizeInBytes, int maxFileCount)
        : m_sink(nullptr)
        , m_slg()
        , m_logDir(logDir)
        , m_logFileName(logFileName)
        , m_singleFileSizeInBytes(singleFileSizeInBytes)
        , m_maxFileCount(maxFileCount)
    {
        init();
    }
    ~Impl() {
        m_sink->flush();
    }
    void write(int level, const char *tag, const char *format, va_list args)
    {
        char buf[1024] = {0};
        int tagLen = strlen(tag);
        strncpy(buf, tag, tagLen);
        buf[tagLen] = ' ';
        buf[tagLen + 1] = ':';
        buf[tagLen + 2] = ' ';
        vsnprintf(buf + tagLen +3, 1024 - tagLen - 3, format, args);
        switch (level)
        {
        case logLevel::INFO:
            BOOST_LOG_SEV(m_slg, normal) << buf;
            printf("INFO    %s\n", buf);
            break;
        case logLevel::ERROR:
            BOOST_LOG_SEV(m_slg, error) << buf;
            printf("ERROR   %s\n", buf);
            break;
        case logLevel::WARNING:
            BOOST_LOG_SEV(m_slg, warning) << buf;
            printf("WARNING %s\n", buf);
            break;
        case logLevel::FATAL:
            BOOST_LOG_SEV(m_slg, fatal) << buf;
            printf("FATAL   %s\n", buf);
            break;
        case logLevel::DEBUG:
            BOOST_LOG_SEV(m_slg, debug) << buf;
            printf("DEBUG   %s\n", buf);
            break;
        case logLevel::VERBOSE:
            BOOST_LOG_SEV(m_slg, verbose) << buf;
            printf("VERBOSE %s\n", buf);
            break;
        default:
            break;
    }
    }
protected:
    void init() {
        // Create a text file sink
        std::string file_name = m_logDir + "/";
        file_name += "%Y%m%d_%H%M%S_%5N.log";
        typedef sinks::synchronous_sink< sinks::text_file_backend > file_sink;
        boost::shared_ptr< file_sink > sink(new file_sink(
            keywords::file_name = file_name.c_str(),                       // file name pattern
            keywords::target_file_name = "%Y%m%d_%H%M%S_%5N.log",   // file name pattern
            keywords::rotation_size = m_singleFileSizeInBytes       // rotation size, in characters
            ));

        // Set up where the rotated files will be stored
        sink->locked_backend()->set_file_collector(sinks::file::make_collector(
            keywords::target = "logs",                              // where to store rotated files
            keywords::max_size = m_singleFileSizeInBytes,                  // maximum total size of the stored files, in bytes
            // keywords::min_free_space = 100 * 1024 * 1024,           // minimum free space on the drive, in bytes
            keywords::max_files = m_maxFileCount                               // maximum number of stored files
            ));

        // Upon restart, scan the target directory for files matching the file_name pattern
        sink->locked_backend()->scan_for_files();

        sink->set_formatter
        (
            expr::format("[%1%] [%2%] [%3%] [%4%] - %5%")
                % expr::attr< boost::posix_time::ptime >("TimeStamp")
                % expr::attr< attrs_log_tid::value_type >("TID")
                % expr::attr< attrs_log_pid::value_type >("PID")
                % expr::attr< severity_level >("Severity")
                % expr::smessage
        );

        // Add it to the core
        logging::core::get()->add_sink(sink);

        // Add some attributes too
        logging::core::get()->add_global_attribute("TimeStamp", attrs::local_clock());
        logging::core::get()->add_global_attribute("TID", attrs_log_tid());
        logging::core::get()->add_global_attribute("PID", attrs_log_pid());

        m_sink = sink;
    }
};

void LogWrapper::initialize(std::string & logDir, std::string & logFileName, int singleFileSizeInBytes, int maxFileCount)
{
    if(m_staticLog)
        return;
    m_staticLog.reset(new LogWrapper);
    m_staticLog->m_impl.reset(new LogWrapper::Impl(logDir, logFileName, singleFileSizeInBytes, maxFileCount));
}

void LogWrapper::getInstanceInitialize(std::string & logDir, std::string & logFileName, int singleFileSizeInBytes, int maxFileCount)
{
    LogWrapper::initialize(logDir,logFileName, singleFileSizeInBytes, maxFileCount);
}

LogWrapper * LogWrapper::getInstance()
{
    return m_staticLog.get();
}

void LogWrapper::log(int level, const char * tag, const char * format, ...)
{
    va_list args;
    va_start(args, format); 
    m_impl->write(level, tag, format, args);
    va_end(args);
}
