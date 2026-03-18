#include "sylar/log.h"
#include "sylar/socket.h"

sylar::Logger::ptr g_logger = SYLAR_LOG_ROOT();

void test_socket()
{
    sylar::IPAddress::ptr addr = sylar::Address::LookupAnyIPAddress("www.baidu.com");
    if (addr)
    {
        SYLAR_LOG_INFO(g_logger) << "LookupAnyIpAddress success,addr = " << addr->toString();
    }
    else
    {
        SYLAR_LOG_ERROR(g_logger) << "LookupAnyIPAddress failed";
    }
    sylar::Socket::ptr sock = sylar::Socket::CreateTCP(addr);
    addr->setPort(80);
    if (!sock->connect(addr))
    {
        SYLAR_LOG_ERROR(g_logger) << "connect failed";
    }
    else
    {
        SYLAR_LOG_INFO(g_logger) << "connect success";
    }

    const char sendMsg[] = "GET / HTTP/1.0\r\n\r\n";
    int send = sock->send(sendMsg, sizeof(sendMsg));
    if (send < 0)
    {
        SYLAR_LOG_ERROR(g_logger) << "send failed";
    }
    std::string resvMsg;
    resvMsg.resize(4096);
    int recv = sock->recv(&resvMsg[0], resvMsg.size());
    if (recv < 0)
    {
        SYLAR_LOG_ERROR(g_logger) << "recv failed";
    }
    else
    {
        resvMsg.resize(recv);
        SYLAR_LOG_INFO(g_logger) << "recv success,recv = " << recv << ",msg = " << resvMsg;
    }
}

int main(int argc, char **argv)
{
    test_socket();
    return 0;
}