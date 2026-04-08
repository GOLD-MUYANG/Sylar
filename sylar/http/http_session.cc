#include "http_session.h"
#include "http_parser.h"

namespace sylar
{
namespace http
{

HttpSession::HttpSession(Socket::ptr sock, bool owner) : SocketStream(sock, owner)
{
}

HttpRequest::ptr HttpSession::recvRequest()
{
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
            return nullptr;
        }
        //表示当前的缓冲区有多少有效字节
        len += offset;
        //返回已解析的字节数
        size_t nparse = parser->execute(data, len);
        if (parser->hasError())
        {
            return nullptr;
        }
        // 更新剩余未解析的数据长度
        offset = len - nparse;
        if (offset == (int)buff_size)
        {
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
        body.reserve(length);

        if (length >= offset)
        {
            body.append(data, offset);
        }
        else
        {
            body.append(data, length);
        }

        length -= offset;
        // 2、如果body还有数据，继续读,这次就是全读出来了（内部有while）
        if (length > 0)
        {
            if (readFixSize(&body[body.size()], length) <= 0)
            {
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
    std::cout << "sendResponse:" << data << std::endl;
    return writeFixSize(data.c_str(), data.size());
}

} // namespace http
} // namespace sylar