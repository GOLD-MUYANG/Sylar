#include "http_server.h"
#include "sylar/log.h"

#include <cerrno>
#include <cstring>

namespace sylar
{
namespace http
{

static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

namespace
{

const char *HttpRecvRequestErrorToString(HttpSessionRecvRequestError error)
{
    switch (error)
    {
    case HttpSessionRecvRequestError::CLIENT_CLOSED:
        return "client_closed";
    case HttpSessionRecvRequestError::TIMEOUT:
        return "timeout";
    case HttpSessionRecvRequestError::READ_ERROR:
        return "read_error";
    case HttpSessionRecvRequestError::PARSE_ERROR:
        return "parse_error";
    case HttpSessionRecvRequestError::REQUEST_TOO_LARGE:
        return "request_too_large";
    case HttpSessionRecvRequestError::NONE:
    default:
        return "unknown";
    }
}

bool IsNormalRecvRequestEnd(HttpSessionRecvRequestError error)
{
    return error == HttpSessionRecvRequestError::CLIENT_CLOSED ||
           error == HttpSessionRecvRequestError::TIMEOUT;
}

} // namespace

HttpServer::HttpServer(bool keepalive, sylar::IOManager *worker, sylar::IOManager *accept_worker)
    : TcpServer(worker, accept_worker), m_isKeepalive(keepalive)
{
    m_dispatch.reset(new ServletDispatch);
}

void HttpServer::setName(const std::string &v)
{
    TcpServer::setName(v);
    // 更新默认的 404 Servlet
    m_dispatch->setDefault(std::make_shared<NotFoundServlet>(v));
}

void HttpServer::handleClient(Socket::ptr client)
{
    HttpSession::ptr session(new HttpSession(client));
    // SYLAR_LOG_INFO(g_logger) << "成功连接上一个客户端" << *client;
    do
    {
        auto req = session->recvRequest();
        if (!req)
        {
            auto error = session->getLastRecvRequestError();
            int error_no = session->getLastRecvRequestErrno();
            if (IsNormalRecvRequestEnd(error))
            {
                SYLAR_LOG_DEBUG(g_logger)
                    << "recv http request end reason=" << HttpRecvRequestErrorToString(error)
                    << " errno=" << error_no << " errstr=" << strerror(error_no)
                    << " client:" << *client;
            }
            else
            {
                SYLAR_LOG_WARN(g_logger)
                    << "recv http request fail reason=" << HttpRecvRequestErrorToString(error)
                    << " errno=" << error_no << " errstr=" << strerror(error_no)
                    << " client:" << *client;
            }
            break;
        }

        HttpResponse::ptr rsp(
            new HttpResponse(req->getVersion(), req->isClose() || !m_isKeepalive));
        rsp->setHeader("Server", getName());
        m_dispatch->handle(req, rsp, session);
        //        rsp->setBody("hello sylar");
        //
        //        SYLAR_LOG_INFO(g_logger) << "requst:" << std::endl
        //            << *req;
        //        SYLAR_LOG_INFO(g_logger) << "response:" << std::endl
        //            << *rsp;
        int rt = session->sendResponse(rsp);
        if (rt <= 0)
        {
            break;
        }

        if (!m_isKeepalive || req->isClose() || rsp->isClose())
        {
            break;
        }
    } while (m_isKeepalive);
    session->close();
}
} // namespace http
} // namespace sylar
