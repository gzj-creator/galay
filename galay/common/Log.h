#ifndef GALAY_LOG_H
#define GALAY_LOG_H

#ifdef ENABLE_DISPLAY_GALAY_LOG
    #define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <memory>
#include <spdlog/spdlog.h>

namespace galay {
class Logger {
public:
    using uptr = std::unique_ptr<Logger>;
    static Logger::uptr createStdoutLoggerST(const std::string& name);
    static Logger::uptr createStdoutLoggerMT(const std::string& name);
    
    static Logger::uptr createDailyFileLoggerST(const std::string& name, const std::string& file_path, int hour, int minute);
    static Logger::uptr createDailyFileLoggerMT(const std::string& name, const std::string& file_path, int hour, int minute);

    static Logger::uptr createRotingFileLoggerST(const std::string& name, const std::string& file_path, size_t max_size, size_t max_files);
    static Logger::uptr createRotingFileLoggerMT(const std::string& name, const std::string& file_path, size_t max_size, size_t max_files);

    Logger(std::shared_ptr<spdlog::logger> logger);
    std::shared_ptr<spdlog::logger> getSpdlogger();
    Logger& pattern(const std::string &pattern);
    Logger& level(spdlog::level::level_enum level);
    ~Logger();
private:
    std::shared_ptr<spdlog::logger> m_logger;
};

}

namespace galay::details {

#define DEFAULT_LOG_QUEUE_SIZE      8192
#define DEFAULT_LOG_THREADS         1

#define DEFAULT_LOG_FILE_PATH       "logs/galay.log"
#define DEFAULT_MAX_LOG_FILE_SIZE   (10 * 1024 * 1024)
#define DEFAULT_MAX_LOG_FILES       3

class InternelLogger {
public: 
    InternelLogger();
    static InternelLogger* getInstance();
    void setLogger(Logger::uptr logger);
    Logger* getLogger();
    static void shutdown();
    ~InternelLogger();
private:
    static std::unique_ptr<InternelLogger> m_instance;
    Logger::uptr m_logger;
    std::shared_ptr<spdlog::details::thread_pool> m_thread_pool;
};


}

namespace galay {

#ifdef GALAY_INTERNEL_LOG
    #define LogTrace(...)       SPDLOG_LOGGER_TRACE(galay::details::InternelLogger::GetInstance()->GetLogger()->SpdLogger(), __VA_ARGS__);\
    #define LogDebug(...)       SPDLOG_LOGGER_DEBUG(galay::details::InternelLogger::getInstance()->getLogger()->getSpdlogger(), __VA_ARGS__)
    #define LogInfo(...)        SPDLOG_LOGGER_INFO(galay::details::InternelLogger::GetInstance()->GetLogger()->SpdLogger(), __VA_ARGS__)
    #define LogWarn(...)        SPDLOG_LOGGER_WARN(galay::details::InternelLogger::GetInstance()->GetLogger()->SpdLogger(), __VA_ARGS__)
    #define LogError(...)       SPDLOG_LOGGER_ERROR(galay::details::InternelLogger::GetInstance()->GetLogger()->SpdLogger(), __VA_ARGS__)
    #define LogCritical(...)    SPDLOG_LOGGER_CRITICAL(galay::details::InternelLogger::GetInstance()->GetLogger()->SpdLogger(), __VA_ARGS__)
#else
    #define LogTrace(...)       (void)0
    #define LogDebug(...)       (void)0
    #define LogInfo(...)        (void)0
    #define LogWarn(...)        (void)0
    #define LogError(...)       (void)0
    #define LogCritical(...)    (void)0
#endif
}

#endif