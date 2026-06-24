#include "sylar/config.h"
#include "sylar/log.h"

#include <fstream>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static sylar::Logger::ptr g_logger = SYLAR_LOG_ROOT();

int main()
{
    char path_template[] = "/tmp/sylar-module-config-timing-XXXXXX";
    char *path = mkdtemp(path_template);
    if (!path)
    {
        SYLAR_LOG_ERROR(g_logger) << "mkdtemp failed";
        return 1;
    }

    const std::string config_path(path);
    const std::string config_file = config_path + "/gateway.yml";
    {
        std::ofstream ofs(config_file);
        ofs << "ai_gateway:\n  enabled: true\n  trace_enabled: false\n";
    }

    if (!sylar::Config::LoadFromConfDir(config_path, true))
    {
        SYLAR_LOG_ERROR(g_logger) << "first config load failed";
        unlink(config_file.c_str());
        rmdir(config_path.c_str());
        return 1;
    }

    auto enabled = sylar::Config::Lookup<bool>("ai_gateway.enabled", false,
                                                "test gateway enabled flag");
    auto trace_enabled = sylar::Config::Lookup<bool>("ai_gateway.trace_enabled", true,
                                                      "test gateway trace flag");
    if (!enabled || !trace_enabled || enabled->getValue() || !trace_enabled->getValue())
    {
        SYLAR_LOG_ERROR(g_logger) << "late-registered config must keep its default before reload";
        unlink(config_file.c_str());
        rmdir(config_path.c_str());
        return 1;
    }

    if (!sylar::Config::LoadFromConfDir(config_path, true) || !enabled->getValue() ||
        trace_enabled->getValue())
    {
        SYLAR_LOG_ERROR(g_logger) << "forced config reload must apply late-registered setting";
        unlink(config_file.c_str());
        rmdir(config_path.c_str());
        return 1;
    }

    unlink(config_file.c_str());
    rmdir(config_path.c_str());
    return 0;
}
