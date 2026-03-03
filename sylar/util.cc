#include "util.h"
#include "sylar/fiber.h"
#include "sylar/log.h"
#include <cstdint>
#include <execinfo.h>
#include <sstream>
#include <sys/time.h>
#include <unistd.h>
#include <vector>
namespace sylar
{

// sylar::Logger::ptr g_logger = SYLAR_LOG_ROOT();

pid_t GetThreadId()
{
    return syscall(SYS_gettid);
}

uint32_t GetFiberId()
{
    return sylar::Fiber::GetFiberId();
}

void Backtrace(std::vector<std::string> &bt, int size, int skip)
{
    void **array = (void **)malloc(sizeof(void *) * size);
    size_t s = ::backtrace(array, size);
    char **strings = backtrace_symbols(array, s);

    for (size_t i = skip; i < s; ++i)
    {
        bt.push_back(strings[i]);
    }

    free(strings);
    free(array);
}
std::string BacktraceToString(int size, int skip, const std::string &prefix)
{
    std::vector<std::string> bt;
    Backtrace(bt, size, skip);
    std::stringstream ss;
    for (size_t i = 0; i < bt.size(); i++)
    {
        ss << prefix << bt[i] << std::endl;
    }
    return ss.str();
}

uint32_t GetFiberID()
{

    return sylar::Fiber::GetFiberId();
}
/**
 * @brief 获取当前时间戳（毫秒）
 * @return 从1970-01-01 00:00:00 UTC到此刻的毫秒数
 */
uint64_t GetCurrentMS()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000ul + tv.tv_usec / 1000;
}

/**
 * @brief 获取当前时间戳（微秒）
 * @return 从1970-01-01 00:00:00 UTC到此刻的微秒数
 */
uint64_t GetCurrentUS()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000 * 1000ul + tv.tv_usec;
}
} // namespace sylar