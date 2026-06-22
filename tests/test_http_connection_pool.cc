#include "sylar/http/http_connection.h"
#include "sylar/log.h"

static sylar::Logger::ptr g_logger = SYLAR_LOG_ROOT();
static int g_failures = 0;

#define EXPECT_TRUE(expr)                                                                          \
    do                                                                                             \
    {                                                                                              \
        if (!(expr))                                                                               \
        {                                                                                          \
            ++g_failures;                                                                          \
            SYLAR_LOG_ERROR(g_logger) << "EXPECT_TRUE failed: " #expr << " line=" << __LINE__;  \
        }                                                                                          \
    } while (0)

int main()
{
    // 工厂方法必须把 URI 解析失败转换为可检查的空指针，而不是继续解引用。
    auto pool = sylar::http::HttpConnectionPool::Create("://invalid", "", 1, 30000, 10);
    EXPECT_TRUE(pool == nullptr);
    return g_failures == 0 ? 0 : 1;
}
