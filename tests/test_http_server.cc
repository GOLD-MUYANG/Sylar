#include "sylar/http/http_server.h"
#include "sylar/http/servlet.h"
#include "sylar/log.h"

static sylar::Logger::ptr g_logger = SYLAR_LOG_ROOT();
#define XX(...) #__VA_ARGS__
void run()
{
    sylar::http::HttpServer::ptr server(new sylar::http::HttpServer(true));
    sylar::Address::ptr addr = sylar::Address::LookupAnyIPAddress("0.0.0.0:8020");
    while (!server->bind(addr))
    {
        sleep(2);
    }

    auto sd = server->getServletDispatch();

    sd->addServlet("/sylar/xx",
                   [](sylar::http::HttpRequest::ptr req, sylar::http::HttpResponse::ptr rsp,
                      sylar::http::HttpSession::ptr session)
                   {
                       rsp->setHeader("Content-Type", "text/plain");
                       rsp->setBody("OK");
                       return 0;
                   });

    sd->addGlobServlet("/sylar/*",
                       [](sylar::http::HttpRequest::ptr req, sylar::http::HttpResponse::ptr rsp,
                          sylar::http::HttpSession::ptr session)
                       {
                           rsp->setHeader("Content-Type", "text/plain");
                           rsp->setBody("OK");
                           return 0;
                       });

    server->start();
}

int main(int argc, char **argv)
{
    sylar::IOManager iom(4);
    iom.schedule(run);
    return 0;
}