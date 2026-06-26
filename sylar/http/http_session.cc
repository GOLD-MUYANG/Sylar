#include "http_session.h"
#include "http_parser.h"

#include <cerrno>

namespace sylar
{
namespace http
{
namespace
{

bool IsTimeoutLikeError(int error)
{
    return error == EAGAIN || error == EWOULDBLOCK || error == ETIMEDOUT;
}

HttpSessionRecvRequestError ClassifyReadFailure(int len, int error)
{
    if (len == 0)
    {
        return HttpSessionRecvRequestError::CLIENT_CLOSED;
    }
    if (IsTimeoutLikeError(error))
    {
        return HttpSessionRecvRequestError::TIMEOUT;
    }
    return HttpSessionRecvRequestError::READ_ERROR;
}

} // namespace

HttpSession::HttpSession(Socket::ptr sock, bool owner) : SocketStream(sock, owner)
{
}

HttpRequest::ptr HttpSession::recvRequest()
{
    m_lastRecvRequestError = HttpSessionRecvRequestError::NONE;
    m_lastRecvRequestErrno = 0;

    HttpRequestParser::ptr parser(new HttpRequestParser);
    // 设置这个主要是为了避免请求体过大，超过一定大小直接认为是非法请求
    uint64_t buff_size = HttpRequestParser::GetHttpRequestBufferSize();
    // uint64_t buff_size = 100;
    std::shared_ptr<char> buffer(new char[buff_size], [](char *ptr) { delete[] ptr; });
    char *data = buffer.get();
    int offset = 0;
    //这里其实就是先读，能读多长就多长（尽量读），让parser尽量解析，但是有可能本次读到的字节没法解析完整的信息，
    //那就下一轮继续读，直到解析完整的信息
    do
    {
        //读到了多少数据，放到data里
        int len = read(data + offset, buff_size - offset);
        if (len <= 0)
        {
            m_lastRecvRequestErrno = len < 0 ? errno : 0;
            m_lastRecvRequestError = ClassifyReadFailure(len, m_lastRecvRequestErrno);
            close();
            return nullptr;
        }
        //表示当前的缓冲区有多少有效字节
        len += offset;
        //返回已解析的字节数
        size_t nparse = parser->execute(data, len);
        if (parser->hasError())
        {
            m_lastRecvRequestError = HttpSessionRecvRequestError::PARSE_ERROR;
            close();
            return nullptr;
        }
        // 更新剩余未解析的数据长度
        offset = len - nparse;
        if (offset == (int)buff_size)
        {
            m_lastRecvRequestError = HttpSessionRecvRequestError::REQUEST_TOO_LARGE;
            close();
            return nullptr;
        }
        if (parser->isFinished())
        {
            break;
        }
    } while (true);

    int64_t length = parser->getContentLength();
    if (length > 0)
    {
        // 1、前面不停地读，本来是想解析头的，但是可能把body也多读出来了，所以先判断是不是有，然后把多读的先写进body变量里
        std::string body;
        body.resize(length);
        int len = 0;
        if (length >= offset)
        {
            // body.append(data, offset);
            memcpy(&body[0], data, offset);
            len = offset;
        }
        else
        {
            // body.append(data, length);
            memcpy(&body[0], data, length);
            len = length;
        }

        length -= offset;
        // 2、如果body还有数据，继续读,这次就是全读出来了（内部有while）
        if (length > 0)
        {
            int read_len = readFixSize(&body[len], length);
            if (read_len <= 0)
            {
                m_lastRecvRequestErrno = read_len < 0 ? errno : 0;
                m_lastRecvRequestError =
                    ClassifyReadFailure(read_len, m_lastRecvRequestErrno);
                close();
                return nullptr;
            }
        }
        parser->getData()->setBody(body);
    }
    return parser->getData();
}

int HttpSession::sendResponse(HttpResponse::ptr rsp)
{
    std::stringstream ss;
    ss << *rsp;
    std::string data = ss.str();
    // std::cout << "sendResponse:" << data << std::endl;
    return writeFixSize(data.c_str(), data.size());
}

} // namespace http
} // namespace sylar
