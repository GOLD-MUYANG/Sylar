#ifndef __SYLAR_LOG_H__
#define __SYLAR_LOG_H__
#include "util.h"
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <list>
#include <memory>
#include <sstream>
#include <stdint.h>
#include <string>
#include <vector>

#define SYLAR_LOG_LEVEL(logger, level)                                                             \
    if (logger->getLevel() <= level)                                                               \
    sylar::LogEventWrap(sylar::LogEvent::ptr(new sylar::LogEvent(                                  \
                            logger, level, __FILE__, __LINE__, 0, sylar::GetThreadId(),            \
                            sylar::GetFiberId(), time(0))))                                        \
        .getSS()

#define SYLAR_LOG_DEBUG(logger) SYLAR_LOG_LEVEL(logger, sylar::LogLevel::DEBUG)
#define SYLAR_LOG_INFO(logger) SYLAR_LOG_LEVEL(logger, sylar::LogLevel::INFO)
#define SYLAR_LOG_WARN(logger) SYLAR_LOG_LEVEL(logger, sylar::LogLevel::WARN)
#define SYLAR_LOG_ERROR(logger) SYLAR_LOG_LEVEL(logger, sylar::LogLevel::ERROR)
#define SYLAR_LOG_FATAL(logger) SYLAR_LOG_LEVEL(logger, sylar::LogLevel::FATAL)

namespace sylar
{

class Logger;

// 日志级别
class LogLevel
{
public:
    enum Level
    {
        UNKNOW = 0,
        DEBUG = 1,
        INFO = 2,
        WARN = 3,
        ERROR = 4,
        FATAL = 5
    };

    static const char *Tostring(LogLevel::Level level);
};

// 日志事件
class LogEvent
{
public:
    typedef std::shared_ptr<LogEvent> ptr;
    LogEvent(std::shared_ptr<Logger> logger, LogLevel::Level level, const char *file, int32_t line,
             uint32_t elapse, int32_t threadId, uint32_t fiberId, int64_t time);
    const char *getFile() const
    {
        return m_file;
    }

    int32_t getLine() const
    {
        return m_line;
    }

    uint32_t getElapse() const
    {
        return m_elapse;
    }
    int32_t getThreadId() const
    {
        return m_threadId;
    }
    uint32_t getFiberId() const
    {
        return m_fiberId;
    }
    int64_t getTime() const
    {
        return m_time;
    }

    std::string getContent() const
    {
        return m_ss.str();
    }

    std::stringstream &getSS()
    {
        return m_ss;
    }

    LogLevel::Level getLevel() const
    {
        return m_level;
    }

    std::shared_ptr<Logger> getLogger() const
    {
        return m_logger;
    }

    void format(const char *fmt, ...);

private:
    const char *m_file = nullptr;     // 日志文件
    int32_t m_line = 0;               // 日志行号
    uint32_t m_elapse = 0;            // 程序启动开始到现在的毫秒数
    int32_t m_threadId = 0;           // 线程ID
    uint32_t m_fiberId = 0;           // 协程ID
    int64_t m_time = 0;               // 时间戳
    std::stringstream m_ss;           // 日志内容
    LogLevel::Level m_level;          // 日志级别
    std::shared_ptr<Logger> m_logger; // 日志器
};

class LogEventWrap
{
public:
    LogEventWrap(LogEvent::ptr e);
    ~LogEventWrap();
    std::stringstream &getSS();

private:
    LogEvent::ptr m_event;
};

// 日志格式器
class LogFormatter
{
public:
    typedef std::shared_ptr<LogFormatter> ptr;
    LogFormatter(const std::string &pattern);
    std::string format(std::shared_ptr<Logger> logger, LogLevel::Level level,
                       const LogEvent::ptr event);

public:
    class FormatItem
    {
    public:
        typedef std::shared_ptr<FormatItem> ptr;
        // FormatItem(const std::string &str = "") {};
        virtual void format(std::ostream &os, std::shared_ptr<Logger> logger, LogLevel::Level level,
                            LogEvent::ptr event) = 0;
    };

    void init();

private:
    std::string m_pattern;
    std::vector<FormatItem::ptr> m_items;
};

// 日志输出地（把日志输出到控制台Or一个文件）
class LogAppender
{
public:
    typedef std::shared_ptr<LogAppender> ptr;
    virtual ~LogAppender()
    {
    }

    virtual void log(std::shared_ptr<Logger> logger, LogLevel::Level level,
                     LogEvent::ptr event) = 0;
    void setFormatter(LogFormatter::ptr val)
    {
        m_formatter = val;
    }

    LogFormatter::ptr getFormatter() const
    {
        return m_formatter;
    }

protected:
    LogLevel::Level m_level = LogLevel::DEBUG;
    LogFormatter::ptr m_formatter;
};

// 日志器
class Logger : public std::enable_shared_from_this<Logger>
{
public:
    typedef std::shared_ptr<Logger> ptr;

    Logger(const std::string &name = "root");

    // 输出不同级别的日志
    void log(LogLevel::Level level, LogEvent::ptr event);
    void debug(LogEvent::ptr event);
    void info(LogEvent::ptr event);
    void warn(LogEvent::ptr event);
    void error(LogEvent::ptr event);
    void fatal(LogEvent::ptr event);

    // 添加和删除日志输出地
    void addAppender(LogAppender::ptr appender);
    void delAppender(LogAppender::ptr appender);
    LogLevel::Level getLevel() const
    {
        return m_level;
    }

    void setLevel(LogLevel::Level val)
    {
        m_level = val;
    }

    const std::string &getName() const
    {
        return m_name;
    }

private:
    std::string m_name;                      // 日志名称
    LogLevel::Level m_level;                 // 日志级别
    std::list<LogAppender::ptr> m_appenders; // 输出地集合
    LogFormatter::ptr m_formatter;
};

// 输出到控制台的Appender
class StdoutLogAppender : public LogAppender
{
public:
    typedef std::unique_ptr<StdoutLogAppender> ptr;
    virtual void log(std::shared_ptr<Logger> logger, LogLevel::Level level,
                     LogEvent::ptr event) override;

private:
}; // namespace sylar

// 输出到文件的Appender
class FileLogAppender : public LogAppender
{
public:
    typedef std::unique_ptr<FileLogAppender> ptr;
    FileLogAppender(const std::string &filename);
    virtual void log(std::shared_ptr<Logger> logger, LogLevel::Level level,
                     LogEvent::ptr event) override;

    // 重新打开文件，成功返回true
    bool reopen();

private:
    std::string m_filename;
    std::ofstream m_fileStream;
};

} // namespace sylar

#endif