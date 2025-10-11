#ifndef GALAY_LOG_H
#define GALAY_LOG_H

#if defined(ENABLE_SYSTEM_LOG) && defined(ENABLE_TRACE)
    #define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <memory>
#include <spdlog/spdlog.h>

namespace galay {
/**
 * @brief 日志记录器类
 * @details 基于spdlog的日志封装，支持多种日志输出方式和格式配置
 */
class Logger {
public:
    using uptr = std::unique_ptr<Logger>;
    
    /**
     * @brief 创建单线程标准输出日志记录器
     * @param name 日志记录器名称
     * @return Logger唯一指针
     */
    static Logger::uptr createStdoutLoggerST(const std::string& name);
    
    /**
     * @brief 创建多线程标准输出日志记录器
     * @param name 日志记录器名称
     * @return Logger唯一指针
     */
    static Logger::uptr createStdoutLoggerMT(const std::string& name);
    
    /**
     * @brief 创建单线程每日滚动文件日志记录器
     * @param name 日志记录器名称
     * @param file_path 日志文件路径
     * @param hour 滚动时间（小时）
     * @param minute 滚动时间（分钟）
     * @return Logger唯一指针
     */
    static Logger::uptr createDailyFileLoggerST(const std::string& name, const std::string& file_path, int hour, int minute);
    
    /**
     * @brief 创建多线程每日滚动文件日志记录器
     * @param name 日志记录器名称
     * @param file_path 日志文件路径
     * @param hour 滚动时间（小时）
     * @param minute 滚动时间（分钟）
     * @return Logger唯一指针
     */
    static Logger::uptr createDailyFileLoggerMT(const std::string& name, const std::string& file_path, int hour, int minute);

    /**
     * @brief 创建单线程大小滚动文件日志记录器
     * @param name 日志记录器名称
     * @param file_path 日志文件路径
     * @param max_size 单个日志文件最大大小（字节）
     * @param max_files 保留的最大日志文件数
     * @return Logger唯一指针
     */
    static Logger::uptr createRotingFileLoggerST(const std::string& name, const std::string& file_path, size_t max_size, size_t max_files);
    
    /**
     * @brief 创建多线程大小滚动文件日志记录器
     * @param name 日志记录器名称
     * @param file_path 日志文件路径
     * @param max_size 单个日志文件最大大小（字节）
     * @param max_files 保留的最大日志文件数
     * @return Logger唯一指针
     */
    static Logger::uptr createRotingFileLoggerMT(const std::string& name, const std::string& file_path, size_t max_size, size_t max_files);

    /**
     * @brief 构造函数
     * @param logger spdlog日志记录器共享指针
     */
    Logger(std::shared_ptr<spdlog::logger> logger);
    
    /**
     * @brief 获取底层spdlog日志记录器
     * @return spdlog::logger共享指针
     */
    std::shared_ptr<spdlog::logger> getSpdlogger();
    
    /**
     * @brief 设置日志输出格式
     * @param pattern 格式字符串（遵循spdlog格式规范）
     * @return Logger引用，支持链式调用
     */
    Logger& pattern(const std::string &pattern);
    
    /**
     * @brief 设置日志级别
     * @param level 日志级别（trace/debug/info/warn/error/critical）
     * @return Logger引用，支持链式调用
     */
    Logger& level(spdlog::level::level_enum level);
    
    ~Logger();
private:
    std::shared_ptr<spdlog::logger> m_logger;
};

}

namespace galay::details {

#define DEFAULT_LOG_QUEUE_SIZE      8192                      ///< 默认日志队列大小
#define DEFAULT_LOG_THREADS         1                         ///< 默认日志线程数

#define DEFAULT_LOG_FILE_PATH       "logs/galay.log"          ///< 默认日志文件路径
#define DEFAULT_MAX_LOG_FILE_SIZE   (10 * 1024 * 1024)       ///< 默认单个日志文件最大大小（10MB）
#define DEFAULT_MAX_LOG_FILES       3                         ///< 默认保留日志文件数

/**
 * @brief 内部日志记录器（单例）
 * @details 管理Galay框架全局日志记录器，支持异步日志处理
 */
class InternelLogger {
public: 
    InternelLogger();
    
    /**
     * @brief 获取单例实例
     * @return InternelLogger指针
     */
    static InternelLogger* getInstance();
    
    /**
     * @brief 设置全局日志记录器
     * @param logger Logger唯一指针
     */
    void setLogger(Logger::uptr logger);
    
    /**
     * @brief 设置全局日志级别
     * @param level 日志级别
     */
    void setLevel(spdlog::level::level_enum level);
    
    /**
     * @brief 获取当前日志记录器
     * @return Logger指针
     */
    Logger* getLogger();
    
    /**
     * @brief 关闭日志系统
     */
    void shutdown();
    
    ~InternelLogger();
private:
    Logger::uptr m_logger;                                               ///< 日志记录器
    std::shared_ptr<spdlog::details::thread_pool> m_thread_pool;        ///< 异步日志线程池
};


}

namespace galay {

#ifdef ENABLE_SYSTEM_LOG
    #define LogTrace(...)       SPDLOG_LOGGER_TRACE(galay::details::InternelLogger::getInstance()->getLogger()->getSpdlogger(), __VA_ARGS__)
    #define LogDebug(...)       SPDLOG_LOGGER_DEBUG(galay::details::InternelLogger::getInstance()->getLogger()->getSpdlogger(), __VA_ARGS__)
    #define LogInfo(...)        SPDLOG_LOGGER_INFO(galay::details::InternelLogger::getInstance()->getLogger()->getSpdlogger(), __VA_ARGS__)
    #define LogWarn(...)        SPDLOG_LOGGER_WARN(galay::details::InternelLogger::getInstance()->getLogger()->getSpdlogger(), __VA_ARGS__)
    #define LogError(...)       SPDLOG_LOGGER_ERROR(galay::details::InternelLogger::getInstance()->getLogger()->getSpdlogger(), __VA_ARGS__)
    #define LogCritical(...)    SPDLOG_LOGGER_CRITICAL(galay::details::InternelLogger::getInstance()->getLogger()->getSpdlogger(), __VA_ARGS__)
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