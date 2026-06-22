#include "sylar/config.h"
#include "sylar/env.h"
#include "sylar/log.h"

#include <limits.h>
#include <string>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

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

#define EXPECT_FALSE(expr)                                                                         \
    do                                                                                             \
    {                                                                                              \
        if (expr)                                                                                  \
        {                                                                                          \
            ++g_failures;                                                                          \
            SYLAR_LOG_ERROR(g_logger) << "EXPECT_FALSE failed: " #expr << " line=" << __LINE__; \
        }                                                                                          \
    } while (0)

namespace
{

std::string current_directory()
{
    char buffer[PATH_MAX] = {0};
    if (!getcwd(buffer, sizeof(buffer)))
    {
        return "";
    }
    return buffer;
}

void test_explicit_config_path_uses_startup_directory()
{
    char program[] = "test_env_config";
    char config_flag[] = "-c";
    char config_path[] = "bin/conf";
    char *argv[] = {program, config_flag, config_path};

    sylar::Env env;
    env.init(3, argv);

    EXPECT_EQ(env.getConfigPath(), current_directory() + "/bin/conf");
}

void test_default_config_path_uses_executable_directory()
{
    char program[] = "test_env_config";
    char *argv[] = {program};

    sylar::Env env;
    env.init(1, argv);

    EXPECT_EQ(env.getConfigPath(), env.getCwd() + "conf");
}

void test_missing_config_directory_is_rejected()
{
    const std::string missing_path = "/tmp/sylar-missing-config-directory";
    EXPECT_FALSE(sylar::Config::LoadFromConfDir(missing_path));
}

void test_empty_config_directory_is_rejected()
{
    char template_path[] = "/tmp/sylar-empty-config-XXXXXX";
    char *empty_path = mkdtemp(template_path);
    if (!empty_path)
    {
        ++g_failures;
        return;
    }

    EXPECT_FALSE(sylar::Config::LoadFromConfDir(empty_path));
    rmdir(empty_path);
}

} // namespace

int main()
{
    test_explicit_config_path_uses_startup_directory();
    test_default_config_path_uses_executable_directory();
    test_missing_config_directory_is_rejected();
    test_empty_config_directory_is_rejected();
    return g_failures == 0 ? 0 : 1;
}
