#include "env.h"
#include "sylar/log.h"
#include <iomanip>
#include <iostream>
#include <stdlib.h> // setenv, getenv
#include <string.h> // strlen
#include <unistd.h> // getpid, readlink

namespace sylar
{

// 日志对象，用于输出错误或调试信息
static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

// 初始化 Env 对象，解析命令行参数
bool Env::init(int argc, char **argv)
{
    // 读取当前程序的实际执行路径
    char link[1024] = {0};
    char path[1024] = {0};
    // /proc/<pid>/exe 是一个 符号链接 (symbolic link)，指向 当前进程的可执行文件的完整路径。
    // 把 /proc/<pid>/exe 这个路径生成到 link 字符数组里。
    sprintf(link, "/proc/%d/exe", getpid());
    // 读取符号链接指向的真实路径,放到path里
    readlink(link, path, sizeof(path)); // 读取实际路径
    m_exe = path;                       // 保存完整程序路径

    // 获取程序所在目录
    auto pos = m_exe.find_last_of("/");
    m_cwd = m_exe.substr(0, pos) + "/"; // 目录路径以 / 结尾

    m_program = argv[0]; // 保存 argv[0]，即程序名

    // 开始解析命令行参数，例如：-config /path/to/config -file xxxx -d
    const char *now_key = nullptr; // 当前解析到的 key
    for (int i = 1; i < argc; ++i)
    {
        if (argv[i][0] == '-') // 遇到 key
        {
            if (strlen(argv[i]) > 1) // key 不能只是 "-"
            {
                if (now_key) // 上一个 key 没有对应 value，设置为空字符串
                {
                    add(now_key, "");
                }
                now_key = argv[i] + 1; // 去掉 "-" 作为 key
            }
            else
            {
                SYLAR_LOG_ERROR(g_logger) << "invalid arg idx=" << i << " val=" << argv[i];
                return false;
            }
        }
        else // 遇到 value
        {
            if (now_key)
            {
                add(now_key, argv[i]); // key -> value
                now_key = nullptr;
            }
            else // 没有 key 却出现 value，是非法参数
            {
                SYLAR_LOG_ERROR(g_logger) << "invalid arg idx=" << i << " val=" << argv[i];
                return false;
            }
        }
    }
    // 如果最后一个 key 没有对应 value，设置为空字符串
    if (now_key)
    {
        add(now_key, "");
    }

    return true; // 初始化成功
}

// 添加命令行参数 key -> value
void Env::add(const std::string &key, const std::string &val)
{
    RWMutexType::WriteLock lock(m_mutex); // 写锁，保证线程安全
    m_args[key] = val;
}

// 判断某个命令行参数是否存在
bool Env::has(const std::string &key)
{
    RWMutexType::ReadLock lock(m_mutex); // 读锁
    auto it = m_args.find(key);
    return it != m_args.end();
}

// 删除某个命令行参数
void Env::del(const std::string &key)
{
    RWMutexType::WriteLock lock(m_mutex);
    m_args.erase(key);
}

// 获取某个命令行参数，如果不存在则返回默认值
std::string Env::get(const std::string &key, const std::string &default_value)
{
    RWMutexType::ReadLock lock(m_mutex);
    auto it = m_args.find(key);
    return it != m_args.end() ? it->second : default_value;
}

// 添加帮助信息
void Env::addHelp(const std::string &key, const std::string &desc)
{
    removeHelp(key); // 先移除已存在的 key，避免重复
    RWMutexType::WriteLock lock(m_mutex);
    m_helps.push_back(std::make_pair(key, desc));
}

// 移除帮助信息
void Env::removeHelp(const std::string &key)
{
    RWMutexType::WriteLock lock(m_mutex);
    for (auto it = m_helps.begin(); it != m_helps.end();)
    {
        if (it->first == key)
        {
            it = m_helps.erase(it); // 删除匹配的 key
        }
        else
        {
            ++it;
        }
    }
}

// 打印帮助信息
void Env::printHelp()
{
    RWMutexType::ReadLock lock(m_mutex);
    std::cout << "Usage: " << m_program << " [options]" << std::endl;
    for (auto &i : m_helps)
    {
        std::cout << std::setw(5) << "-" << i.first << " : " << i.second << std::endl;
    }
}

// 设置环境变量
bool Env::setEnv(const std::string &key, const std::string &val)
{
    // setenv 返回 0 表示成功，因此用 ! 转换为 bool
    return !setenv(key.c_str(), val.c_str(), 1);
}

// 获取环境变量，如果不存在则返回默认值
std::string Env::getEnv(const std::string &key, const std::string &default_value)
{
    const char *v = getenv(key.c_str());
    if (v == nullptr)
    {
        return default_value;
    }
    return v;
}

std::string Env::getAbsolutePath(const std::string &path) const
{
    if (path.empty())
    {
        return "/";
    }
    if (path[0] == '/')
    {
        return path;
    }
    return m_cwd + path;
}

std::string Env::getConfigPath()
{
    return getAbsolutePath(get("c", "conf"));
}
} // namespace sylar