#ifndef __SYLAR_TCP_SERVER_H__
#define __SYLAR_TCP_SERVER_H__

#include "address.h"
#include "iomanager.h"
#include "noncopyable.h"
#include "socket.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sylar
{

class TcpServer : public std::enable_shared_from_this<TcpServer>, Noncopyable
{
public:
    typedef std::shared_ptr<TcpServer> ptr;
    TcpServer(sylar::IOManager *woker = sylar::IOManager::GetThis(),
              sylar::IOManager *accept_woker = sylar::IOManager::GetThis());
    virtual ~TcpServer();

    //将socket和一个地址绑定，这个地址就是服务器的地址，用来和客户端连接
    virtual bool bind(sylar::Address::ptr addr, bool ssl = false);
    //可以管理多个socket，给定一个地址，完成socket的accept之前的所有动作
    virtual bool bind(const std::vector<Address::ptr> &addrs,
                      std::vector<Address::ptr> &fails,
                      bool ssl = false);
    bool loadCertificates(const std::string &cert_file, const std::string &key_file);
    virtual bool start();
    virtual void stop();

    uint64_t getRecvTimeout() const
    {
        return m_recvTimeout;
    }
    std::string getName() const
    {
        return m_name;
    }
    void setRecvTimeout(uint64_t v)
    {
        m_recvTimeout = v;
    }
    virtual void setName(const std::string &v)
    {
        m_name = v;
    }

protected:
    //子类进行重写，当连接上一个客户端时做一些动作
    virtual void handleClient(Socket::ptr client);
    virtual void startAccept(Socket::ptr sock);

private:
    std::vector<Socket::ptr> m_socks;
    IOManager *m_worker;
    IOManager *m_acceptWorker;
    uint64_t m_recvTimeout;
    std::string m_name;
    bool m_isStop;
};
} // namespace sylar
#endif