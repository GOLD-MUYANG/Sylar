#include "sylar/application.h"
#include "sylar/log.h"

static sylar::Logger::ptr g_logger = SYLAR_LOG_ROOT();

int main()
{
    sylar::Application application;
    if (application.getHttpServer("gateway") != nullptr)
    {
        SYLAR_LOG_ERROR(g_logger) << "unknown HTTP server must not be created";
        return 1;
    }
    return 0;
}
