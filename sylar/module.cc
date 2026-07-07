#include "module.h"
#include "config.h"
#include "env.h"
#include "library.h"
#include "util.h"

namespace sylar
{

static sylar::ConfigVar<std::string>::ptr g_module_path =
    Config::Lookup("module.path", std::string("module"), "module path");
static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

Module::Module(const std::string &name, const std::string &version, const std::string &filename)
    : m_name(name), m_version(version), m_filename(filename), m_id(name + "/" + version)
{
}

// 模块提前注册自己支持的命令行参数
void Module::onBeforeArgsParse(int argc, char **argv)
{
}
// 模块根据解析后的参数做初始化准备
void Module::onAfterArgsParse(int argc, char **argv)
{
}

bool Module::onLoad()
{
    return true;
}

bool Module::onUnload()
{
    return true;
}

bool Module::onConnect(sylar::Stream::ptr stream)
{
    return true;
}

bool Module::onDisconnect(sylar::Stream::ptr stream)
{
    return true;
}

bool Module::onServerReady()
{
    return true;
}

bool Module::onServerUp()
{
    return true;
}
std::string Module::statusString()
{
    return "";
}
ModuleManager::ModuleManager()
{
}

Module::ptr ModuleManager::get(const std::string &name)
{
    RWMutexType::ReadLock lock(m_mutex);
    auto it = m_modules.find(name);
    return it == m_modules.end() ? nullptr : it->second;
}

void ModuleManager::add(Module::ptr m)
{
    del(m->getId());
    RWMutexType::WriteLock lock(m_mutex);
    m_modules[m->getId()] = m;
}

void ModuleManager::del(const std::string &name)
{
    Module::ptr module;
    RWMutexType::WriteLock lock(m_mutex);
    auto it = m_modules.find(name);
    if (it == m_modules.end())
    {
        return;
    }
    module = it->second;
    m_modules.erase(it);
    lock.unlock();
    module->onUnload();
}

void ModuleManager::delAll()
{
    RWMutexType::ReadLock lock(m_mutex);
    auto tmp = m_modules;
    lock.unlock();

    for (auto &i : tmp)
    {
        del(i.first);
    }
}

/**
从模块目录加载所有模块
*/
void ModuleManager::init()
{
    // 1. 找到模块目录
    auto path = EnvMgr::GetInstance()->getAbsolutePath(g_module_path->getValue());

    // 2. 找出所有 .so 文件
    std::vector<std::string> files;
    sylar::FSUtil::ListAllFile(files, path, ".so");

    // 3. 排序，保证加载顺序稳定
    std::sort(files.begin(), files.end());

    // 4. 逐个加载
    for (auto &i : files)
    {
        initModule(i);
    }
}

void ModuleManager::onBeforeArgsParse(int argc, char **argv)
{
    std::vector<Module::ptr> ms;
    listAll(ms);

    for (auto &m : ms)
    {
        try
        {
            m->onBeforeArgsParse(argc, argv);
        }
        catch (const std::exception &e)
        {
            SYLAR_LOG_ERROR(g_logger)
                << "module onBeforeArgsParse threw id=" << m->getId() << " error=" << e.what();
        }
        catch (...)
        {
            SYLAR_LOG_ERROR(g_logger) << "module onBeforeArgsParse threw id=" << m->getId();
        }
    }
}

void ModuleManager::onAfterArgsParse(int argc, char **argv)
{
    std::vector<Module::ptr> ms;
    listAll(ms);

    for (auto &m : ms)
    {
        try
        {
            m->onAfterArgsParse(argc, argv);
        }
        catch (const std::exception &e)
        {
            SYLAR_LOG_ERROR(g_logger)
                << "module onAfterArgsParse threw id=" << m->getId() << " error=" << e.what();
        }
        catch (...)
        {
            SYLAR_LOG_ERROR(g_logger) << "module onAfterArgsParse threw id=" << m->getId();
        }
    }
}

void ModuleManager::onConnect(Stream::ptr stream)
{
    std::vector<Module::ptr> ms;
    listAll(ms);

    for (auto &m : ms)
    {
        try
        {
            if (!m->onConnect(stream))
            {
                SYLAR_LOG_ERROR(g_logger) << "module onConnect failed id=" << m->getId();
            }
        }
        catch (const std::exception &e)
        {
            SYLAR_LOG_ERROR(g_logger)
                << "module onConnect threw id=" << m->getId() << " error=" << e.what();
        }
        catch (...)
        {
            SYLAR_LOG_ERROR(g_logger) << "module onConnect threw id=" << m->getId();
        }
    }
}

void ModuleManager::onDisconnect(Stream::ptr stream)
{
    std::vector<Module::ptr> ms;
    listAll(ms);

    for (auto &m : ms)
    {
        try
        {
            if (!m->onDisconnect(stream))
            {
                SYLAR_LOG_ERROR(g_logger) << "module onDisconnect failed id=" << m->getId();
            }
        }
        catch (const std::exception &e)
        {
            SYLAR_LOG_ERROR(g_logger)
                << "module onDisconnect threw id=" << m->getId() << " error=" << e.what();
        }
        catch (...)
        {
            SYLAR_LOG_ERROR(g_logger) << "module onDisconnect threw id=" << m->getId();
        }
    }
}

void ModuleManager::onServerReady()
{
    std::vector<Module::ptr> ms;
    listAll(ms);

    for (auto &m : ms)
    {
        try
        {
            if (!m->onServerReady())
            {
                SYLAR_LOG_ERROR(g_logger) << "module onServerReady failed id=" << m->getId();
            }
        }
        catch (const std::exception &e)
        {
            SYLAR_LOG_ERROR(g_logger)
                << "module onServerReady threw id=" << m->getId() << " error=" << e.what();
        }
        catch (...)
        {
            SYLAR_LOG_ERROR(g_logger) << "module onServerReady threw id=" << m->getId();
        }
    }
}

void ModuleManager::onServerUp()
{
    std::vector<Module::ptr> ms;
    listAll(ms);

    for (auto &m : ms)
    {
        try
        {
            if (!m->onServerUp())
            {
                SYLAR_LOG_ERROR(g_logger) << "module onServerUp failed id=" << m->getId();
            }
        }
        catch (const std::exception &e)
        {
            SYLAR_LOG_ERROR(g_logger)
                << "module onServerUp threw id=" << m->getId() << " error=" << e.what();
        }
        catch (...)
        {
            SYLAR_LOG_ERROR(g_logger) << "module onServerUp threw id=" << m->getId();
        }
    }
}

void ModuleManager::listAll(std::vector<Module::ptr> &ms)
{
    RWMutexType::ReadLock lock(m_mutex);
    for (auto &i : m_modules)
    {
        ms.push_back(i.second);
    }
}

void ModuleManager::initModule(const std::string &path)
{
    Module::ptr m = Library::GetModule(path);
    if (!m)
    {
        return;
    }

    if (!m->onLoad())
    {
        SYLAR_LOG_ERROR(g_logger) << "module onLoad failed name=" << m->getName()
                                  << " version=" << m->getVersion() << " path=" << path;
        return;
    }

    add(m);
}
} // namespace sylar
