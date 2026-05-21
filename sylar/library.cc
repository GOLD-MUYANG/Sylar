#include "library.h"

#include "sylar/config.h"
#include "sylar/env.h"
#include "sylar/log.h"
#include <dlfcn.h>

namespace sylar
{

static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

// 模块动态库需要导出的创建函数，Library 通过该函数构造 Module 实例。
typedef Module *(*create_module)();

// 模块动态库需要导出的销毁函数，用于和 CreateModule 成对释放模块对象。
typedef void (*destory_module)(Module *);

// ModuleCloser 作为 Module::ptr 的自定义删除器，负责销毁模块并关闭动态库句柄。
class ModuleCloser
{
public:
    ModuleCloser(void *handle, destory_module d) : m_handle(handle), m_destory(d)
    {
    }

    void operator()(Module *module)
    {
        // 先保存模块信息，避免模块对象销毁后日志无法再访问这些字段。
        std::string name = module->getName();
        std::string version = module->getVersion();
        std::string path = module->getFilename();
        m_destory(module);
        int rt = dlclose(m_handle);
        if (rt)
        {
            SYLAR_LOG_ERROR(g_logger)
                << "dlclose handle fail handle=" << m_handle << " name=" << name
                << " version=" << version << " path=" << path << " error=" << dlerror();
        }
        else
        {
            SYLAR_LOG_INFO(g_logger) << "destory module=" << name << " version=" << version
                                     << " path=" << path << " handle=" << m_handle << " success";
        }
    }

private:
    void *m_handle;
    destory_module m_destory;
};

Module::ptr Library::GetModule(const std::string &path)
{
    // 尝试打开 path 指向的 .so 文件，如果失败，打印错误并返回 nullptr
    void *handle = dlopen(path.c_str(), RTLD_NOW);
    if (!handle)
    {
        SYLAR_LOG_ERROR(g_logger) << "cannot load library path=" << path << " error=" << dlerror();
        return nullptr;
    }
    // library.cc 里定义了两个函数指针类型：
    // 会查找两个符号。也就是说，每个模块 .so 必须提供这下面两个函数：

    // 约定模块动态库必须导出 CreateModule 作为模块入口。
    create_module create = (create_module)dlsym(handle, "CreateModule");
    if (!create)
    {
        SYLAR_LOG_ERROR(g_logger) << "cannot load symbol CreateModule in " << path
                                  << " error=" << dlerror();
        dlclose(handle);
        return nullptr;
    }

    // 约定模块动态库必须导出 DestoryModule，供 shared_ptr 删除器释放模块。
    destory_module destory = (destory_module)dlsym(handle, "DestoryModule");
    if (!destory)
    {
        SYLAR_LOG_ERROR(g_logger) << "cannot load symbol DestoryModule in " << path
                                  << " error=" << dlerror();
        dlclose(handle);
        return nullptr;
    }

    // 将动态库句柄绑定到模块指针的删除器中，模块释放时同步调用 dlclose。
    Module::ptr module(create(), ModuleCloser(handle, destory));
    module->setFilename(path);
    SYLAR_LOG_INFO(g_logger) << "load module name=" << module->getName()
                             << " version=" << module->getVersion()
                             << " path=" << module->getFilename() << " success";
    // 模块加载后重新扫描配置目录，让模块相关配置立即生效。
    Config::LoadFromConfDir(sylar::EnvMgr::GetInstance()->getConfigPath(), true);
    return module;
}

} // namespace sylar
