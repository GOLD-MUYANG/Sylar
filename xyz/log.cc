#include "log.h"
#include <cctype>
#include <cstddef>
#include <iostream>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace sylar
{

const char *LogLevel::Tostring(LogLevel::Level level)
{
    switch (level)
    {

#define XX(name)                                                                                   \
    case LogLevel::name:                                                                           \
        return #name;                                                                              \
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
Logger::Logger(const std::string &name) : m_name(name)
{
}

void Logger::addAppender(LogAppender::ptr appender)
{
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
        for (auto &i : m_appenders)
        {
            i->log(level, event);
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
    : m_filename(filename) {

      };

void FileLogAppender::log(LogLevel::Level level, LogEvent::ptr event)
{
    if (level >= m_level)
    {
        m_fileStream << m_formatter->format(event);
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
void StdoutLogAppender::log(LogLevel::Level level, LogEvent::ptr event)
{
    if (level >= m_level)
    {
        std::cout << m_formatter->format(level, event);
    }
};

LogFormatter::LogFormatter(const std::string &pattern)
    : m_pattern(pattern) {

      };

std::string LogFormatter::format(LogLevel::Level level, LogEvent::ptr event)
{
    std::stringstream ss;
    for (auto i : m_items)
    {
        i->format(ss, level, event);
    }
    return ss.str();
};

// 需要格式化的字符串为 %xxx{xxx} 或者 %xxx xxx xxx %xxx xxx
void LogFormatter::init()
{
    std::vector<std::tuple<std::string, std::string, int>> vec;
    size_t last_pos = 0;
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
        size_t n = i + 1; // n是正常文本信息的下一个位置
        int fmt_status = 0;
        std::string str; // str 是 {}之前的字符串
        std::string fmt; // fmt 是 {} 里面的字符串
        size_t fmt_begin = 0;
        while (n < m_pattern.size())
        {
            // 如果有空格符，说明就到了下一个需要格式化的位置
            if (isspace(m_pattern[n]))
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
        }
        // 没有遇到格式化“%”符号
        if (fmt_status == 0)
        {
            if (!nstr.empty())
            {
                vec.push_back(std::make_tuple(nstr, "", 0));
            }
            str = m_pattern.substr(i + 1, n - i - 1);
            vec.push_back(std::make_tuple(str, fmt, 1));
            i = n; // 到正常文本的下一个位置
        }
        // 如果格式化符号解析错误
        else if (fmt_status == 1)
        {
            std::cout << "pattern parse error:" << m_pattern << "-" << m_pattern.substr(i)
                      << std::endl;
            vec.push_back(std::make_tuple("<<pattern_error>>", fmt, 0));
        }
        // 如果遇到解析化符号{}而且解析成功
        else if (fmt_status == 2)
        {
            vec.push_back(std::make_tuple(str, fmt, 1));
            i = n; // 到{}的下一个需要判断的位置
        }
    }
    if (!nstr.empty())
    {
        vec.push_back(std::make_tuple(nstr, "", 0));
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

    class MessageFormatItem : public LogFormatter::FormatItem
    {
    public:
        void format(std::ostream &os, LogLevel::Level level, LogEvent::ptr event) override
        {
            os << event->getContent();
        };
    };

    class LevelFormatItem : public LogFormatter::FormatItem
    {
    public:
        void format(std::ostream &os, LogLevel::Level level, LogEvent::ptr event) override
        {
            os << LogLevel::Tostring(level);
        };
    };
};

} // namespace sylar