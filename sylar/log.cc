#include "log.h"
#include <bits/types/time_t.h>
#include <cctype>
#include <cstddef>
#include <ctime>
#include <functional>
#include <map>
#include <string>
#include <time.h>

namespace sylar
{

const char *LogLevel::Tostring(LogLevel::Level level)
{
    switch (level)
    {

#define XX(name)                                                                                                                                     \
    case LogLevel::name:                                                                                                                             \
        return #name;                                                                                                                                \
        break;
        XX(DEBUG);
        XX(INFO);
        XX(WARN);
        XX(ERROR);
        XX(FATAL);
#undef XX
    default:
        return "UNKNOW";
    }
    return "UNKNOW";
};

class MessageFormatItem : public LogFormatter::FormatItem
{
public:
    MessageFormatItem(const std::string &str = "")
    {
    }
    void format(std::ostream &os, std::shared_ptr<Logger> logger, LogLevel::Level level, LogEvent::ptr event) override
    {
        os << event->getContent();
    };
};

class LevelFormatItem : public LogFormatter::FormatItem
{
public:
    LevelFormatItem(const std::string &str = "")
    {
    }
    void format(std::ostream &os, std::shared_ptr<Logger> logger, LogLevel::Level level, LogEvent::ptr event) override
    {
        os << LogLevel::Tostring(level);
    };
};

class ElapseFormatItem : public LogFormatter::FormatItem
{
public:
    ElapseFormatItem(const std::string &str = "")
    {
    }
    void format(std::ostream &os, std::shared_ptr<Logger> logger, LogLevel::Level level, LogEvent::ptr event) override
    {
        os << event->getElapse();
    };
};

class NameFormatItem : public LogFormatter::FormatItem
{
public:
    NameFormatItem(const std::string &str = "")
    {
    }
    void format(std::ostream &os, std::shared_ptr<Logger> logger, LogLevel::Level level, LogEvent::ptr event) override
    {
        os << logger->getName();
        // os << event->getElapse();
    };
};

class ThreadIdFormatItem : public LogFormatter::FormatItem
{
public:
    ThreadIdFormatItem(const std::string &str = "")
    {
    }
    void format(std::ostream &os, std::shared_ptr<Logger> logger, LogLevel::Level level, LogEvent::ptr event) override
    {

        os << event->getThreadId();
    };
};

class FiberIdFormatItem : public LogFormatter::FormatItem
{
public:
    FiberIdFormatItem(const std::string &str = "")
    {
    }
    void format(std::ostream &os, std::shared_ptr<Logger> logger, LogLevel::Level level, LogEvent::ptr event) override
    {

        os << event->getFiberId();
    };
};

class DateTimeFormatItem : public LogFormatter::FormatItem
{
public:
    DateTimeFormatItem(const std::string &format = "%Y-%m-%d %H:%M:%S") : m_format(format)
    {
        if (m_format.empty())
        {
            m_format = "%Y-%m-%d %H:%M:%S";
        }
    }
    void format(std::ostream &os, std::shared_ptr<Logger> logger, LogLevel::Level level, LogEvent::ptr event) override
    {
        struct tm tm;
        time_t time = event->getTime();
        localtime_r(&time, &tm);
        char buf[64];
        strftime(buf, sizeof(buf), m_format.c_str(), &tm);
        // os << event->getTime();
        os << buf;
    };

private:
    std::string m_format;
};

class FilenameFormatItem : public LogFormatter::FormatItem
{
public:
public:
    FilenameFormatItem(const std::string &str = "")
    {
    }
    void format(std::ostream &os, std::shared_ptr<Logger> logger, LogLevel::Level level, LogEvent::ptr event) override
    {

        os << event->getFile();
    };
};

class LineFormatItem : public LogFormatter::FormatItem
{
public:
    LineFormatItem(const std::string &str = "")
    {
    }
    void format(std::ostream &os, std::shared_ptr<Logger> logger, LogLevel::Level level, LogEvent::ptr event) override
    {

        os << event->getLine();
    };
};

class NewLineFormatItem : public LogFormatter::FormatItem
{
public:
    NewLineFormatItem(const std::string &str = "")
    {
    }
    void format(std::ostream &os, std::shared_ptr<Logger> logger, LogLevel::Level level, LogEvent::ptr event) override
    {

        os << std::endl;
    };
};

class StringFormatItem : public LogFormatter::FormatItem
{
public:
    StringFormatItem(const std::string &str) : m_string(str)
    {
    }
    void format(std::ostream &os, std::shared_ptr<Logger> logger, LogLevel::Level level, LogEvent::ptr event) override
    {

        os << m_string;
    };

private:
    std::string m_string;
};

LogEvent::LogEvent(const char *file, int32_t line, uint32_t elapse, int32_t threadId, uint32_t fiberId, int64_t time)
    : m_file(file), m_line(line), m_elapse(elapse), m_threadId(threadId), m_fiberId(fiberId), m_time(time){};

Logger::Logger(const std::string &name) : m_name(name), m_level(LogLevel::DEBUG)
{
    // 如果没有传递格式化参数，就使用默认的格式化参数
    m_formatter.reset(new LogFormatter("%d [%p] <%f:%l>      %m %n"));
}

void Logger::addAppender(LogAppender::ptr appender)
{
    // 使用默认的格式化参数
    if (!appender->getFormatter())
    {
        appender->setFormatter(m_formatter);
    }
    m_appenders.push_back(appender);
};

void Logger::delAppender(LogAppender::ptr appender)
{
    for (auto it = m_appenders.begin(); it != m_appenders.end(); it++)
    {
        if (*it == appender)
        {
            m_appenders.erase(it);
            break;
        }
    }
};
void Logger::log(LogLevel::Level level, LogEvent::ptr event)
{
    if (level >= m_level)
    {
        auto self = shared_from_this();
        for (auto &i : m_appenders)
        {
            i->log(self, level, event);
        }
    }
};

void Logger::debug(LogEvent::ptr event)
{
    log(LogLevel::DEBUG, event);
}

void Logger::info(LogEvent::ptr event)
{
    log(LogLevel::INFO, event);
};

void Logger::warn(LogEvent::ptr event)
{
    log(LogLevel::WARN, event);
};

void Logger::error(LogEvent::ptr event)
{
    log(LogLevel::ERROR, event);
};

void Logger::fatal(LogEvent::ptr event)
{
    log(LogLevel::FATAL, event);
};
FileLogAppender::FileLogAppender(const std::string &filename)
    : m_filename(filename){

      };

void FileLogAppender::log(std::shared_ptr<Logger> logger, LogLevel::Level level, LogEvent::ptr event)
{
    if (level >= m_level)
    {
        m_fileStream << m_formatter->format(logger, level, event);
    }
};

// 如果文件已经打开，那么就先把他关闭，再重新打开
bool FileLogAppender::reopen()
{
    if (m_fileStream)
    {
        m_fileStream.close();
    }
    m_fileStream.open(m_filename);
    return !!m_fileStream;
};
void StdoutLogAppender::log(std::shared_ptr<Logger> logger, LogLevel::Level level, LogEvent::ptr event)
{
    if (level >= m_level)
    {
        std::string str = m_formatter->format(logger, level, event);
        std::cout << str;
    }
};

LogFormatter::LogFormatter(const std::string &pattern) : m_pattern(pattern)
{
    init();
};

std::string LogFormatter::format(std::shared_ptr<Logger> logger, LogLevel::Level level, LogEvent::ptr event)
{
    std::stringstream ss;
    for (auto i : m_items)
    {
        i->format(ss, logger, level, event);
    }
    return ss.str();
};

// 需要格式化的字符串为%d [%p] <%f:%l> %m %n
void LogFormatter::init()
{
    // 第一个string表示需要格式化的字符串，第二个string表示格式化的模板，第三个int表示是否有模板
    std::vector<std::tuple<std::string, std::string, int>> vec;
    // size_t last_pos = 0;
    std::string nstr;
    for (size_t i = 0; i < m_pattern.size(); ++i)
    {
        if (m_pattern[i] != '%')
        {
            nstr.append(1, m_pattern[i]);
            continue;
        }
        // 只处理%%的情况，第二个%就是正常的文本信息，就nstr加一个%进去
        if ((i + 1) < m_pattern.size())
        {
            if (m_pattern[i + 1] == '%')
            {

                nstr.append(1, '%');
                continue;
            }
        }

        // 只有当遇到一个单独的%，才会执行下面的代码 如下的格式xxx%xxx{xxx}
        // 先把%之前的字符串存起来
        size_t n = i + 1; // n是正常文本信息(跳过当前的%)
        int fmt_status = 0;
        std::string str; // str 是 {}之前的字符串
        std::string fmt; // fmt 是 {} 里面的字符串
        size_t fmt_begin = 0;
        while (n < m_pattern.size())
        {
            // 如果有空格符，说明就到了下一个需要格式化的位置
            if (!std::isalpha(m_pattern[n]) && m_pattern[n] != '{' && m_pattern[n] != '}')
            {
                break;
            }

            // 遇到需要格式化的地方
            if (m_pattern[n] == '{')
            {
                str = m_pattern.substr(i + 1, n - i - 1);
                ++n;
                fmt_status = 1; // 遇到了{
                fmt_begin = n;
                continue;
            }

            // 遇到结束格式化的符号
            if (fmt_status == 1)
            {
                if (m_pattern[n] == '}')
                {
                    fmt = m_pattern.substr(fmt_begin + 1, n - fmt_begin - 1);
                    fmt_status = 2;
                    ++n; // 到}的下一个位置
                    break;
                }
            }
            ++n;
        }
        // 遇到%但是没有{}，就正常格式化输出
        if (fmt_status == 0)
        {
            if (!nstr.empty())
            {
                vec.push_back(std::make_tuple(nstr, std::string(""), 0));
                nstr.clear();
            }
            str = m_pattern.substr(i + 1, n - i - 1);
            vec.push_back(std::make_tuple(str, fmt, 1));
            i = n - 1; // 到正常文本的下一个位置
        }
        // 如果格式化符号解析错误
        else if (fmt_status == 1)
        {
            std::cout << "pattern parse error:" << m_pattern << "-" << m_pattern.substr(i) << std::endl;
            vec.push_back(std::make_tuple("<<pattern_error>>", fmt, 0));
        }
        // 如果遇到解析化符号{}而且解析成功
        else if (fmt_status == 2)
        {
            if (!nstr.empty())
            {
                vec.push_back(std::make_tuple(nstr, std::string(""), 0));
                nstr.clear();
            }
            vec.push_back(std::make_tuple(str, fmt, 1));
            i = n - 1; // 到{}的下一个需要判断的位置
        }
    }
    if (!nstr.empty())
    {
        vec.push_back(std::make_tuple(nstr, std::string(""), 0));
    }

    static std::map<std::string, std::function<FormatItem::ptr(const std::string &)>> s_format_items = {
#define XX(str, C)                                                                                                                                   \
    {                                                                                                                                                \
#str, [](const std::string &fmt) { return FormatItem::ptr(new C(fmt)); }                                                                     \
    }
        XX(m, MessageFormatItem),  XX(p, LevelFormatItem),   XX(r, ElapseFormatItem),   XX(c, NameFormatItem),
        XX(t, ThreadIdFormatItem), XX(n, NewLineFormatItem), XX(d, DateTimeFormatItem), XX(f, FilenameFormatItem),
        XX(l, LineFormatItem),     XX(F, FiberIdFormatItem)
#undef XX
    };

    // std::tuple<std::string, std::string, int>
    for (auto &i : vec)
    {
        if (std::get<2>(i) == 0)
        {
            m_items.push_back(FormatItem::ptr(new StringFormatItem(std::get<0>(i))));
        }
        else
        {
            auto it = s_format_items.find(std::get<0>(i));
            if (it == s_format_items.end())
            {
                m_items.push_back(FormatItem::ptr(new StringFormatItem("<<error_format %" + std::get<0>(i) + ">>")));
            }
            else
            {
                m_items.push_back(it->second(std::get<1>(i)));
            }
        }
        std::cout << "(" << std::get<0>(i) << ")-(" << std::get<1>(i) << "),(" << std::get<2>(i) << ")" << std::endl;
    }
}

// %m -- 消息体
// %p -- 日志级别
// %r -- 累计毫秒数
// %c -- 日志名称
// %t -- 线程id
// %n -- 换行
// %d -- 时间
// %f -- 文件名
// %l -- 行号
// %T -- 制表符
// %F -- 协程id
}; // namespace sylar

// namespace sylar