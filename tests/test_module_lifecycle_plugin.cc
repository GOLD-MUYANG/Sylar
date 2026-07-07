#include "sylar/module.h"

#include <sstream>

namespace
{

class LifecycleModule : public sylar::Module
{
public:
    LifecycleModule() : Module("lifecycle", "1.0", "")
    {
    }

    bool onLoad() override
    {
        ++m_loadCount;
        return true;
    }

    void onBeforeArgsParse(int, char **) override
    {
        ++m_beforeArgsParseCount;
    }

    void onAfterArgsParse(int, char **) override
    {
        ++m_afterArgsParseCount;
    }

    bool onUnload() override
    {
        ++m_unloadCount;
        return true;
    }

    bool onServerReady() override
    {
        ++m_readyCount;
        return true;
    }

    bool onServerUp() override
    {
        ++m_upCount;
        return true;
    }

    bool onConnect(sylar::Stream::ptr) override
    {
        ++m_connectCount;
        return true;
    }

    bool onDisconnect(sylar::Stream::ptr) override
    {
        ++m_disconnectCount;
        return true;
    }

    std::string statusString() override
    {
        std::stringstream ss;
        ss << "load=" << m_loadCount << ";before_args=" << m_beforeArgsParseCount
           << ";after_args=" << m_afterArgsParseCount << ";ready=" << m_readyCount
           << ";up=" << m_upCount << ";connect=" << m_connectCount
           << ";disconnect=" << m_disconnectCount << ";unload=" << m_unloadCount;
        return ss.str();
    }

private:
    uint32_t m_loadCount = 0;
    uint32_t m_beforeArgsParseCount = 0;
    uint32_t m_afterArgsParseCount = 0;
    uint32_t m_readyCount = 0;
    uint32_t m_upCount = 0;
    uint32_t m_connectCount = 0;
    uint32_t m_disconnectCount = 0;
    uint32_t m_unloadCount = 0;
};

} // namespace

extern "C"
{

sylar::Module *CreateModule()
{
    return new LifecycleModule;
}

void DestoryModule(sylar::Module *module)
{
    delete module;
}

}
