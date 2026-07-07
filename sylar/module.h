#ifndef __SYLAR_MODULE_H__
#define __SYLAR_MODULE_H__

#include "sylar/mutex.h"
#include "sylar/singleton.h"
#include "sylar/stream.h"
#include <map>

namespace sylar
{
class Module
{
public:
    typedef std::shared_ptr<Module> ptr;
    Module(const std::string &name, const std::string &version, const std::string &filename);
    virtual ~Module()
    {
    }

    virtual void onBeforeArgsParse(int argc, char **argv);
    virtual void onAfterArgsParse(int argc, char **argv);

    // 模块加载时调用 onLoad 成功才 add 到 ModuleManager
    virtual bool onLoad();

    // 模块卸载时调用
    virtual bool onUnload();

    // 每个 TCP 连接进来和断开时通知模块。
    virtual bool onConnect(Stream::ptr stream);

    // 连接断开时调用
    virtual bool onDisconnect(Stream::ptr stream);

    // HTTP server 已经创建 / bind 好，但还没开始 accept，适合注册 servlet 路由。
    virtual bool onServerReady();

    // 服务器真正启动后调用
    // 上报服务状态
    // 启动后台任务
    // 启动健康检查
    // 启动定时器
    // 向注册中心注册自己
    virtual bool onServerUp();

    virtual std::string statusString();

    const std::string &getName() const
    {
        return m_name;
    }
    const std::string &getVersion() const
    {
        return m_version;
    }
    const std::string &getFilename() const
    {
        return m_filename;
    }
    const std::string &getId() const
    {
        return m_id;
    }

    void setFilename(const std::string &v)
    {
        m_filename = v;
    }

protected:
    std::string m_name;
    std::string m_version;
    std::string m_filename;
    std::string m_id;
};

class ModuleManager
{
public:
    typedef RWMutex RWMutexType;

    ModuleManager();

    void add(Module::ptr m);
    void del(const std::string &name);
    void delAll();

    void init();

    Module::ptr get(const std::string &name);

    void onBeforeArgsParse(int argc, char **argv);
    void onAfterArgsParse(int argc, char **argv);
    void onConnect(Stream::ptr stream);
    void onDisconnect(Stream::ptr stream);
    void onServerReady();
    void onServerUp();
    void listAll(std::vector<Module::ptr> &ms);

private:
    void initModule(const std::string &path);

private:
    RWMutexType m_mutex;
    std::map<std::string, Module::ptr> m_modules;
};

typedef sylar::Singleton<ModuleManager> ModuleMgr;

} // namespace sylar
#endif
