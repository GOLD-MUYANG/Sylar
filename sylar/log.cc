#include "log.h"
#include "config.h"
#include <bits/types/time_t.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <time.h>
#include <vector>

namespace sylar
{

const char *LogLevel::toString(LogLevel::Level level)
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

LogLevel::Level LogLevel::fromString(const std::string &str)
{
#define XX(level, v)                                                                               \
    if (str == #v)                                                                                 \
    {                                                                                              \
        return LogLevel::level;                                                                    \
    }
    XX(DEBUG, debug);
    XX(INFO, info);
    XX(WARN, warn);
    XX(ERROR, error);
    XX(FATAL, fatal);

    XX(DEBUG, DEBUG);
    XX(INFO, INFO);
    XX(WARN, WARN);
    XX(ERROR, ERROR);
    XX(FATAL, FATAL);
    return LogLevel::UNKNOW;
#undef XX
}

LogEventWrap::LogEventWrap(LogEvent::ptr e) : m_event(e) {}
LogEventWrap::~LogEventWrap()
{
    if (m_event)
    {
        m_event->getLogger()->log(m_event->getLevel(), m_event);
    }
}
std::stringstream &LogEventWrap::getSS() { return m_event->getSS(); }

void LogAppender::setFormatter(LogFormatter::ptr val)
{
    MutexType::Lock lock(m_mutex);
    m_formatter = val;
    if (m_formatter)
    {
        m_hasFormatter = true;
    }
    else
    {
        m_hasFormatter = false;
    }
}

LogFormatter::ptr LogAppender::getFormatter()
{
    MutexType::Lock lock(m_mutex);
    return m_formatter;
}

void LogEvent::format(const char *fmt, ...)
{
    va_list al;
    va_start(al, fmt);
    format(fmt, al);
    va_end(al);
}

void LogEvent::format(const char *fmt, va_list al)
{
    char *buf = nullptr;
    int len = vasprintf(&buf, fmt, al);
    if (len != -1)
    {
        m_ss << std::string(buf, len);
        free(buf);
    }
}

class MessageFormatItem : public LogFormatter::FormatItem
{
public:
    MessageFormatItem(const std::string &str = "") {}
    void format(std::ostream &os,
                Logger::ptr logger,
                LogLevel::Level level,
                LogEvent::ptr event) override
    {
        os << event->getContent();
    };
};

class LevelFormatItem : public LogFormatter::FormatItem
{
public:
    LevelFormatItem(const std::string &str = "") {}
    void format(std::ostream &os,
                Logger::ptr logger,
                LogLevel::Level level,
                LogEvent::ptr event) override
    {
        os << LogLevel::toString(level);
    };
};

class ElapseFormatItem : public LogFormatter::FormatItem
{
public:
    ElapseFormatItem(const std::string &str = "") {}
    void format(std::ostream &os,
                Logger::ptr logger,
                LogLevel::Level level,
                LogEvent::ptr event) override
    {
        os << event->getElapse();
    };
};

class NameFormatItem : public LogFormatter::FormatItem
{
public:
    NameFormatItem(const std::string &str = "") {}
    void format(std::ostream &os,
                Logger::ptr logger,
                LogLevel::Level level,
                LogEvent::ptr event) override
    {
        os << event->getLogger()->getName();
    };
};

class ThreadIdFormatItem : public LogFormatter::FormatItem
{
public:
    ThreadIdFormatItem(const std::string &str = "") {}
    void format(std::ostream &os,
                Logger::ptr logger,
                LogLevel::Level level,
                LogEvent::ptr event) override
    {

        os << event->getThreadId();
    };
};

class FiberIdFormatItem : public LogFormatter::FormatItem
{
public:
    FiberIdFormatItem(const std::string &str = "") {}
    void format(std::ostream &os,
                Logger::ptr logger,
                LogLevel::Level level,
                LogEvent::ptr event) override
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
    void format(std::ostream &os,
                Logger::ptr logger,
                LogLevel::Level level,
                LogEvent::ptr event) override
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
    FilenameFormatItem(const std::string &str = "") {}
    void format(std::ostream &os,
                Logger::ptr logger,
                LogLevel::Level level,
                LogEvent::ptr event) override
    {

        os << event->getFile();
    };
};

class LineFormatItem : public LogFormatter::FormatItem
{
public:
    LineFormatItem(const std::string &str = "") {}
    void format(std::ostream &os,
                Logger::ptr logger,
                LogLevel::Level level,
                LogEvent::ptr event) override
    {

        os << event->getLine();
    };
};

class NewLineFormatItem : public LogFormatter::FormatItem
{
public:
    NewLineFormatItem(const std::string &str = "") {}
    void format(std::ostream &os,
                Logger::ptr logger,
                LogLevel::Level level,
                LogEvent::ptr event) override
    {

        os << std::endl;
    };
};

class StringFormatItem : public LogFormatter::FormatItem
{
public:
    StringFormatItem(const std::string &str) : m_string(str) {}
    void format(std::ostream &os,
                Logger::ptr logger,
                LogLevel::Level level,
                LogEvent::ptr event) override
    {

        os << m_string;
    };

private:
    std::string m_string;
};

class TabFormatItem : public LogFormatter::FormatItem
{
public:
    TabFormatItem(const std::string &str = "") {}
    void format(std::ostream &os,
                Logger::ptr logger,
                LogLevel::Level level,
                LogEvent::ptr event) override
    {

        os << "\t"; // 制表符
    };

private:
    std::string m_string;
};

LogEvent::LogEvent(Logger::ptr logger,
                   LogLevel::Level level,
                   const char *file,
                   int32_t line,
                   uint32_t elapse,
                   int32_t threadId,
                   uint32_t fiberId,
                   int64_t time)
    : m_file(file), m_line(line), m_elapse(elapse), m_threadId(threadId), m_fiberId(fiberId),
      m_time(time), m_level(level), m_logger(logger){};

Logger::Logger(const std::string &name) : m_name(name), m_level(LogLevel::DEBUG)
{
    // 使用默认的格式化参数
    m_formatter.reset(
        new LogFormatter("%d{%Y-%m-%d %H:%M:%S}%T%t%T%F%T[%p]%T[%c]%T<%f:%l>%T%m%T%n"));
}

void Logger::addAppender(LogAppender::ptr appender)
{
    MutexType::Lock lock(m_mutex);
    // 使用默认的格式化参数
    if (!appender->getFormatter())
    {
        MutexType::Lock appender_lock(appender->m_mutex);
        appender->m_formatter = m_formatter;
    }
    m_appenders.push_back(appender);
};

void Logger::delAppender(LogAppender::ptr appender)
{
    MutexType::Lock lock(m_mutex);
    for (auto it = m_appenders.begin(); it != m_appenders.end(); it++)
    {
        if (*it == appender)
        {
            m_appenders.erase(it);
            break;
        }
    }
};

void Logger::clearAppenders()
{
    MutexType::Lock lock(m_mutex);
    m_appenders.clear();
}

void Logger::log(LogLevel::Level level, LogEvent::ptr event)
{

    if (level >= m_level)
    {
        auto self = shared_from_this();
        MutexType::Lock lock(m_mutex);
        if (!m_appenders.empty())
        {
            for (auto &i : m_appenders)
            {
                i->log(self, level, event);
            }
        }
        else if (m_root)
        {
            // MutexType::Lock lock(m_mutex);
            m_root->log(level, event);
        }
    }
};
void Logger::setFormatter(LogFormatter::ptr val)
{
    MutexType::Lock lock(m_mutex);
    m_formatter = val;
    for (auto &i : m_appenders)
    {
        MutexType::Lock appender_lock(i->m_mutex);
        if (!i->m_hasFormatter)
        {
            i->m_formatter = m_formatter;
        }
    }
}

void Logger::setFormatter(const std::string &val)
{
    sylar::LogFormatter::ptr new_val(new sylar::LogFormatter(val));
    if (new_val->isError())
    {
        SYLAR_LOG_ERROR(SYLAR_LOG_ROOT())
            << "Logger setFormatter name=" << m_name << " invalid formatter "
            << "value = " << val << "invlid formatter";
        return;
    }
    setFormatter(new_val);
}

LogFormatter::ptr Logger::getFormatter()
{
    MutexType::Lock lock(m_mutex);
    return m_formatter;
}

void Logger::debug(LogEvent::ptr event) { log(LogLevel::DEBUG, event); }

void Logger::info(LogEvent::ptr event) { log(LogLevel::INFO, event); };

void Logger::warn(LogEvent::ptr event) { log(LogLevel::WARN, event); };

void Logger::error(LogEvent::ptr event) { log(LogLevel::ERROR, event); };

void Logger::fatal(LogEvent::ptr event) { log(LogLevel::FATAL, event); };
FileLogAppender::FileLogAppender(const std::string &filename) : m_filename(filename) { reopen(); };

void FileLogAppender::log(Logger::ptr logger, LogLevel::Level level, LogEvent::ptr event)
{

    if (level >= m_level)
    {
        uint64_t now = time(0);
        if (now != m_lastTime)
        {
            reopen();
            m_lastTime = now;
        }
        MutexType::Lock lock(m_mutex);
        if (!(m_fileStream << m_formatter->format(logger, level, event)))
        {
            std::cout << "error" << std::endl;
        }
    }
};
std::string FileLogAppender::toYamlString()
{
    MutexType::Lock lock(m_mutex);
    YAML::Node node;
    node["type"] = "FileLogAppender";
    node["file"] = m_filename;
    if (m_level != LogLevel::UNKNOW)
    {
        node["level"] = LogLevel::toString(m_level);
    }
    if (m_hasFormatter && m_formatter)
    {
        node["formatter"] = m_formatter->getPattern();
    }
    std::stringstream ss;
    ss << node;
    return ss.str();
}

// 如果文件已经打开，那么就先把他关闭，再重新打开
bool FileLogAppender::reopen()
{
    MutexType::Lock lock(m_mutex);
    if (m_fileStream)
    {
        m_fileStream.close();
    }
    m_fileStream.open(m_filename);
    return !!m_fileStream;
};
void StdoutLogAppender::log(Logger::ptr logger, LogLevel::Level level, LogEvent::ptr event)
{
    if (level >= m_level)
    {
        MutexType::Lock lock(m_mutex);
        std::string str = m_formatter->format(logger, level, event);
        std::cout << str;
    }
};

std::string StdoutLogAppender::toYamlString()
{
    MutexType::Lock lock(m_mutex);
    YAML::Node node;
    node["type"] = "StdoutLogAppender";
    if (m_level != LogLevel::UNKNOW)
    {
        node["level"] = LogLevel::toString(m_level);
    }
    if (m_hasFormatter && m_formatter)
    {
        node["formatter"] = m_formatter->getPattern();
    }
    std::stringstream ss;
    ss << node;
    return ss.str();
}

LogFormatter::LogFormatter(const std::string &pattern) : m_pattern(pattern) { init(); };

std::string LogFormatter::format(Logger::ptr logger, LogLevel::Level level, LogEvent::ptr event)
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
            // 在大括号里遇到非字母字符也处理
            if (!fmt_status && !std::isalpha(m_pattern[n]) && m_pattern[n] != '{' &&
                m_pattern[n] != '}')
            {
                break;
            }

            // 遇到需要格式化的地方
            if (m_pattern[n] == '{')
            {
                str = m_pattern.substr(i + 1, n - i - 1);
                fmt_status = 1; // 遇到了{
                fmt_begin = n;
                ++n;
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
            // std::cout << "pattern parse error:" << m_pattern << "-" <<
            // m_pattern.substr(i) << std::endl;
            m_error = true;
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

    static std::map<std::string, std::function<FormatItem::ptr(const std::string &)>>
        s_format_items = {
#define XX(str, C)                                                                                 \
    {                                                                                              \
#str, [](const std::string &fmt) { return FormatItem::ptr(new C(fmt)); }                   \
    }
            XX(m, MessageFormatItem),  XX(p, LevelFormatItem),    XX(r, ElapseFormatItem),
            XX(c, NameFormatItem),     XX(t, ThreadIdFormatItem), XX(n, NewLineFormatItem),
            XX(d, DateTimeFormatItem), XX(f, FilenameFormatItem), XX(l, LineFormatItem),
            XX(F, FiberIdFormatItem),  XX(T, TabFormatItem)
#undef XX
        };
    // vec里是tuple
    // tuple第一个参数是格式化字符串的所属类别（比如// %m -- 消息体，%p --
    // 日志级别，%r -- 累计毫秒数），
    // 第二个是格式化的模板，只有DateTimeFormatItem会用到其实
    // 第三个是是否是最普通的字符串（0就是普通字符串，1就是需要格式化输出的）
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
                m_items.push_back(FormatItem::ptr(
                    new StringFormatItem("<<error_format %" + std::get<0>(i) + ">>")));
                m_error = true;
            }
            else
            {
                m_items.push_back(it->second(std::get<1>(i)));
            }
        }
    }
    // std::cout << "(" << std::get<0>(i) << ")-(" << std::get<1>(i) <<
    // "),(" << std::get<2>(i)
    // << ")" << std::endl;
}
LoggerManager::LoggerManager()
{
    m_root.reset(new Logger);
    m_root->addAppender(LogAppender::ptr(new sylar::StdoutLogAppender));
    m_loggers[m_root->m_name] = m_root;
    init();
}

std::string LoggerManager::toYamlString()
{
    MutexType::Lock lock(m_mutex);
    YAML::Node node;
    for (auto &i : m_loggers)
    {
        node.push_back(YAML::Load(i.second->toYamlString()));
    }
    std::stringstream ss;
    ss << node;
    return ss.str();
}

std::string Logger::toYamlString()
{
    MutexType::Lock lock(m_mutex);
    YAML::Node node;
    node["name"] = m_name;
    if (m_level != LogLevel::UNKNOW)
    {
        node["level"] = LogLevel::toString(m_level);
    }
    if (m_formatter)
    {
        node["formatter"] = m_formatter->getPattern();
    }

    for (auto &i : m_appenders)
    {
        node["appenders"].push_back(YAML::Load(i->toYamlString()));
    }
    std::stringstream ss;
    ss << node;
    return ss.str();
}

Logger::ptr LoggerManager::getLogger(const std::string &name)
{
    MutexType::Lock lock(m_mutex);
    auto it = m_loggers.find(name);
    if (it != m_loggers.end())
    {
        return it->second;
    }
    Logger::ptr logger(new Logger(name));
    logger->m_root = m_root;
    m_loggers[name] = logger;
    return logger;
}

struct LogAppenderDefine
{
    int type = 0; // 1 File 2 Stdout
    LogLevel::Level level = LogLevel::DEBUG;
    std::string formatter;
    std::string file;

    bool operator==(const LogAppenderDefine &oth) const
    {
        return type == oth.type && level == oth.level && formatter == oth.formatter &&
               file == oth.file;
    }
};

struct LogDefine
{
    std::string name;
    LogLevel::Level level;
    std::string formatter;
    std::vector<LogAppenderDefine> appenders;

    bool operator==(const LogDefine &oth) const
    {
        return name == oth.name && level == oth.level && formatter == oth.formatter &&
               appenders == oth.appenders;
    }

    bool operator<(const LogDefine &oth) const { return name < oth.name; }
};

template <>
class LexicalCast<std::string, std::set<LogDefine>>
{
public:
    std::set<LogDefine> operator()(const std::string &v)
    {
        YAML::Node node = YAML::Load(v);
        std::set<LogDefine> vec;
        // node["name"].IsDefined() IsDefined()方法用于检查YAML节点是否已定义
        for (size_t i = 0; i < node.size(); ++i)
        {
            auto n = node[i];
            if (!n["name"].IsDefined())
            {
                std::cout << "log config error: name is null, " << n << std::endl;
                continue;
            }

            LogDefine ld;
            ld.name = n["name"].as<std::string>();
            ld.level =
                LogLevel::fromString(n["level"].IsDefined() ? n["level"].as<std::string>() : "");
            if (n["formatter"].IsDefined())
            {
                ld.formatter = n["formatter"].as<std::string>();
            }

            if (n["appenders"].IsDefined())
            {
                // std::cout << "==" << ld.name << " = " << n["appenders"].size() << std::endl;
                for (size_t x = 0; x < n["appenders"].size(); ++x)
                {
                    auto a = n["appenders"][x];
                    if (!a["type"].IsDefined())
                    {
                        std::cout << "log config error: appender type is null, " << a << std::endl;
                        continue;
                    }
                    std::string type = a["type"].as<std::string>();
                    LogAppenderDefine lad;
                    if (type == "FileLogAppender")
                    {
                        lad.type = 1;
                        if (!a["file"].IsDefined())
                        {
                            std::cout << "log config error: fileappender file is null, " << a
                                      << std::endl;
                            continue;
                        }
                        lad.file = a["file"].as<std::string>();
                        if (a["formatter"].IsDefined())
                        {
                            lad.formatter = a["formatter"].as<std::string>();
                        }
                    }
                    else if (type == "StdoutLogAppender")
                    {
                        lad.type = 2;
                    }
                    else
                    {
                        std::cout << "log config error: appender type is invalid, " << a
                                  << std::endl;
                        continue;
                    }

                    ld.appenders.push_back(lad);
                }
            }
            // std::cout << "---" << ld.name << " - "
            //           << ld.appenders.size() << std::endl;
            vec.insert(ld);
        }
        return vec;
    }
};

template <>
class LexicalCast<std::set<LogDefine>, std::string>
{
public:
    std::string operator()(const std::set<LogDefine> &v)
    {
        YAML::Node node;
        for (auto &i : v)
        {
            YAML::Node n;
            n["name"] = i.name;
            if (i.level != LogLevel::UNKNOW)
            {
                n["level"] = LogLevel::toString(i.level);
            }
            if (i.formatter.empty())
            {
                n["formatter"] = i.formatter;
            }

            for (auto &a : i.appenders)
            {
                YAML::Node na;
                if (a.type == 1)
                {
                    na["type"] = "FileLogAppender";
                    na["file"] = a.file;
                }
                else if (a.type == 2)
                {
                    na["type"] = "StdoutLogAppender";
                }
                if (a.level != LogLevel::UNKNOW)
                {
                    na["level"] = LogLevel::toString(a.level);
                }

                if (!a.formatter.empty())
                {
                    na["formatter"] = a.formatter;
                }

                n["appenders"].push_back(na);
            }
            node.push_back(n);
        }
        std::stringstream ss;
        ss << node;
        return ss.str();
    }
};

//用LookUp就是把数据写到ConfigVarMap里，全局只关注这一个logs就行了，把整个配置文件读过来生成相应的日志系统变量
sylar::ConfigVar<std::set<LogDefine>>::ptr g_log_defines =
    sylar::Config::Lookup("logs", std::set<LogDefine>(), "logs config");

struct LogIniter
{
    LogIniter()
    {
        g_log_defines->addListener(
            [](const std::set<LogDefine> &old_value, const std::set<LogDefine> &new_value)
            {
                SYLAR_LOG_INFO(SYLAR_LOG_ROOT()) << "on_logger_conf_changed";
                for (auto &i : new_value)
                {
                    auto it = old_value.find(i);
                    sylar::Logger::ptr logger;
                    //新增，新的有，老的没有
                    if (it == old_value.end())
                    {
                        //配置文件里写的里是新增的，那就新增logger,并且加到LoggerManager里
                        logger = SYLAR_LOG_NAME(i.name);
                    }
                    //新旧都有，则进行修改的操作
                    else
                    {
                        if (!(i == *it))
                        {
                            //修改logger
                            logger = SYLAR_LOG_NAME(i.name);
                        }
                    }
                    logger->setLevel(i.level);
                    //如果配置文件没给formatter，就用默认的
                    if (!i.formatter.empty())
                    {
                        logger->setFormatter(i.formatter);
                    }
                    //先干掉所有，再根据配置文件添加appender
                    logger->clearAppenders();
                    for (auto &a : i.appenders)
                    {

                        sylar::LogAppender::ptr ap;
                        // LogDefine里面的type是1就是File，2就是Stdout
                        if (a.type == 1)
                        {
                            ap.reset(new FileLogAppender(a.file));
                        }
                        else if (a.type == 2)
                        {
                            ap.reset(new StdoutLogAppender);
                        }
                        ap->setLevel(a.level);
                        if (!a.formatter.empty())
                        {
                            sylar::LogFormatter::ptr fmt(new sylar::LogFormatter(a.formatter));
                            if (!fmt->isError())
                            {
                                ap->setFormatter(fmt);
                            }
                            else
                            {
                                SYLAR_LOG_ERROR(SYLAR_LOG_ROOT())
                                    << "invalid appender formatter: " << a.formatter;
                                ap->setFormatter(logger->getFormatter());
                            }
                        }
                        else
                        {
                            // 继承logger的formatter
                            ap->setFormatter(logger->getFormatter());
                        }
                        logger->addAppender(ap);
                    }
                }
                //删除
                for (auto &i : old_value)
                {
                    auto it = new_value.find(i);
                    if (it == new_value.end())
                    {
                        //删除logger(不做真正的删除，只是关闭文件，修改等级这样的操作)
                        auto logger = SYLAR_LOG_NAME(i.name);
                        logger->setLevel((LogLevel::Level)100);
                        logger->clearAppenders();
                    }
                }
            });
    }
};

static LogIniter __log_init;

void LoggerManager::init() {}

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