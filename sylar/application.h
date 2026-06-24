#ifndef __SYLAR_APPLICATION_H__
#define __SYLAR_APPLICATION_H__

#include "sylar/http/http_server.h"

namespace sylar
{

class Application
{
public:
    Application();

    static Application *GetInstance()
    {
        return s_instance;
    }
    bool init(int argc, char **argv);
    bool run();

    /**
     * @brief 在 onServerReady 阶段按名称取得已完成 bind 的 HTTP 服务。
     *
     * 模块只能使用返回的 HttpServer 注册业务路由，不能通过该接口创建、
     * 替换或启动服务。
     */
    sylar::http::HttpServer::ptr getHttpServer(const std::string &name) const;

private:
    int main(int argc, char **argv);
    int run_fiber();

private:
    int m_argc = 0;
    char **m_argv = nullptr;

    std::vector<sylar::http::HttpServer::ptr> m_httpservers;
    static Application *s_instance;
    IOManager::ptr m_mainIOManager;
};

} // namespace sylar

#endif
