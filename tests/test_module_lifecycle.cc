#include "sylar/config.h"
#include "sylar/env.h"
#include "sylar/log.h"
#include "sylar/module.h"
#include "sylar/socket_stream.h"

#include <string>

static sylar::Logger::ptr g_logger = SYLAR_LOG_ROOT();
static int g_failures = 0;

#define EXPECT_EQ(lhs, rhs)                                                                        \
    do                                                                                             \
    {                                                                                              \
        const std::string _lhs = (lhs);                                                           \
        const std::string _rhs = (rhs);                                                           \
        if (_lhs != _rhs)                                                                          \
        {                                                                                          \
            ++g_failures;                                                                          \
            SYLAR_LOG_ERROR(g_logger) << "EXPECT_EQ failed: " #lhs "=" << _lhs << " " #rhs "=" \
                                      << _rhs << " line=" << __LINE__;                             \
        }                                                                                          \
    } while (0)

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
    char program[] = "test_module_lifecycle";
    char *argv[] = {program};
    EXPECT_TRUE(sylar::EnvMgr::GetInstance()->init(1, argv));

    auto module_path = sylar::Config::Lookup<std::string>("module.path", "", "test module path");
    module_path->setValue(
        sylar::EnvMgr::GetInstance()->getCwd() + "test_module_lifecycle_plugins");

    sylar::ModuleManager manager;
    manager.init();
    auto module = manager.get("lifecycle/1.0");
    EXPECT_TRUE(module != nullptr);
    if (!module)
    {
        return 1;
    }

    EXPECT_EQ(module->statusString(), "load=1;ready=0;up=0;connect=0;disconnect=0;unload=0");
    manager.onServerReady();
    manager.onServerUp();
    auto stream = std::make_shared<sylar::SocketStream>(nullptr, false);
    manager.onConnect(stream);
    manager.onDisconnect(stream);
    EXPECT_EQ(module->statusString(), "load=1;ready=1;up=1;connect=1;disconnect=1;unload=0");
    manager.delAll();
    EXPECT_EQ(module->statusString(), "load=1;ready=1;up=1;connect=1;disconnect=1;unload=1");
    module.reset();

    return g_failures == 0 ? 0 : 1;
}
