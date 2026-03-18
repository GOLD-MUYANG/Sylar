#ifndef __SYLAR_SOCKET_H__
#define __SYLAR_SOCKET_H__

#include "address.h"
#include "noncopyable.h"
#include <memory>
namespace sylar
{
class Socket
{
public:
    typedef std::shared_ptr<Socket> ptr;
    typedef std::weak_ptr<Socket> weak_ptr;

    enum Type
    {
        TCP = SOCK_STREAM,
        UDP = SOCK_DGRAM
    };

    enum Family
    {
        IPv4 = AF_INET,
        IPv6 = AF_INET6,
        Unix = AF_UNIX
    };

    static Socket::ptr CreateTCP(Address::ptr addr);
    static Socket::ptr CreateUDP(Address::ptr addr);

    static Socket::ptr CreateTCPSocket();
    static Socket::ptr CreateUDPSocket();

    static Socket::ptr CreateTCPSocket6();
    static Socket::ptr CreateUDPSocket6();

    static Socket::ptr CreateUnixTCPSocket();
    static Socket::ptr CreateUnixUDPSocket();

    Socket(int family, int type, int protocal);
    ~Socket();

    int64_t getSendTimeOut() const;
    void setSendTimeOut(int64_t v);

    int64_t getRecvTimeOut() const;
    void setRecvTimeOut(int64_t v);

    bool getOption(int level, int optname, void *optval, size_t *optlen);
    template <class T>
    bool getOption(int level, int optname, T &result)
    {
        socklen_t length = sizeof(result);
        return getOption(level, optname, &result, &length);
    }

    bool setOption(int level, int option, const void *result, size_t len);
    template <class T>
    bool setOption(int level, int option, const T &value)
    {
        return setOption(level, option, &value, sizeof(T));
    }

    Socket::ptr accpet();

    bool bind(const Address::ptr addr);
    bool connect(const Address::ptr addr, uint64_t timeout_ms = -1);
    // 表示你希望这个监听 socket 最多挂 128 个待处理连接。
    bool listen(int backlog = SOMAXCONN);
    bool close();

    // TCP
    int send(const void *buffer, size_t len, int flags = 0);
    int send(const iovec *buffer, size_t len, int flags = 0);
    // UDP,因为UDP是无连接的，所以接法消息得带一个地址
    int sendTo(const void *buffer, size_t len, const Address::ptr to, int flags = 0);
    int sendTo(const iovec *buffer, size_t len, const Address::ptr to, int flags = 0);

    int recv(void *buffer, size_t len, int flags = 0);
    int recv(iovec *buffer, size_t len, int flags = 0);
    int recvFrom(void *buffer, size_t len, Address::ptr from, int flags = 0);
    int recvFrom(iovec *buffer, size_t len, Address::ptr from, int flags = 0);

    Address::ptr getLocalAddress();
    Address::ptr getRemoteAddress();

    int getFamily() const
    {
        return m_family;
    }
    int getType() const
    {
        return m_type;
    }
    int getProtocol() const
    {
        return m_protocol;
    }

    bool isConnected() const
    {
        return m_isConnected;
    }
    bool isValid() const;
    int getError();

    int getSocket() const
    {
        return m_sock;
    }

    bool cancelRead();
    bool cancelWrite();
    bool cancelAccept();
    bool cancelAll();

    // 就是打印socket的信息
    std::ostream &dump(std::ostream &os) const;

private:
    bool init(int sock);
    void newSock();
    void initSock();

private:
    int m_sock;
    int m_family;
    int m_type;
    int m_protocol;
    bool m_isConnected;

    Address::ptr m_localAddress;
    Address::ptr m_remoteAddress;
};

} // namespace sylar
#endif