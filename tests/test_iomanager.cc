
#include "iostream"
#include "sylar/iomanager.h"
#include "sylar/sylar.h"
#include "sylar/timer.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
sylar::Logger::ptr g_logger = SYLAR_LOG_ROOT();
int sock = 0;
//测试对scheduler的继承是否还有用
void test_fiber()
{
    SYLAR_LOG_INFO(g_logger) << "test_fiber sock=" << sock;
    // socket
    // 参数： ipv4 tcp 0
    sock = socket(AF_INET, SOCK_STREAM, 0);
    //设置非阻塞
    fcntl(sock, F_SETFL, O_NONBLOCK);

    //配置要连接的服务端
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(80);
    inet_pton(AF_INET, "183.2.172.17", &addr.sin_addr.s_addr);
    // connect
    //直接连接成功
    if (!connect(sock, (const struct sockaddr *)&addr, sizeof(addr)))
    {
    }
    //连接失败，被阻塞，加入到iomanager中，等待连接完成
    else if (errno == EINPROGRESS)
    {
        SYLAR_LOG_INFO(g_logger) << "add event errno=" << errno << " " << strerror(errno);
        //加入到iomanager中，等待连接完成
        sylar::IOManager::GetThis()->addEvent(
            sock, sylar::IOManager::READ, []() { SYLAR_LOG_INFO(g_logger) << "read callback"; });
        sylar::IOManager::GetThis()->addEvent(sock, sylar::IOManager::WRITE,
                                              []()
                                              {
                                                  SYLAR_LOG_INFO(g_logger) << "write callback";
                                                  sylar::IOManager::GetThis()->cancelEvent(
                                                      sock, sylar::IOManager::READ);
                                                  close(sock);
                                              });
    }
    else
    {
        SYLAR_LOG_INFO(g_logger) << "else " << errno << " " << strerror(errno);
    }

    // read/write
}

// io manager test
void test1()
{
    std::cout << "EPOLLIN=" << EPOLLIN << " EPOLLOUT=" << EPOLLOUT << std::endl;
    sylar::IOManager iom;
    iom.schedule(&test_fiber);
}

sylar::Timer::ptr s_timer;
void test_timer()
{
    sylar::IOManager iom;
    s_timer = iom.addTimer(
        1000,
        []()
        {
            static int i = 0;
            SYLAR_LOG_INFO(g_logger) << "hello timer i=" << i;
            if (++i == 3)
            {
                s_timer->reset(2000, true);
                s_timer->cancel();
            }
        },
        true);
}

int main(int argc, char **argv)
{
    test1();
    return 0;
}