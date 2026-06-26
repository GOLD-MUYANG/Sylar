#ifndef __SYLAR_HTTP_SESSION_H__
#define __SYLAR_HTTP_SESSION_H__

#include "http.h"
#include "sylar/socket_stream.h"

namespace sylar
{
namespace http
{
enum class HttpSessionRecvRequestError
{
    NONE = 0,
    CLIENT_CLOSED,
    TIMEOUT,
    READ_ERROR,
    PARSE_ERROR,
    REQUEST_TOO_LARGE
};

class HttpSession : public SocketStream
{
public:
    typedef std::shared_ptr<HttpSession> ptr;
    HttpSession(Socket::ptr sock, bool owner = true);
    HttpRequest::ptr recvRequest();
    HttpSessionRecvRequestError getLastRecvRequestError() const
    {
        return m_lastRecvRequestError;
    }
    int getLastRecvRequestErrno() const
    {
        return m_lastRecvRequestErrno;
    }
    int sendResponse(HttpResponse::ptr rsp);

private:
    HttpSessionRecvRequestError m_lastRecvRequestError = HttpSessionRecvRequestError::NONE;
    int m_lastRecvRequestErrno = 0;
};

} // namespace http
} // namespace sylar

#endif
