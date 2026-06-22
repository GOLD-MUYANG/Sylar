#include "socket.h"
#include "fd_manager.h"
#include "hook.h"
#include "iomanager.h"
#include "log.h"
#include "macro.h"
#include <cstddef>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>

namespace sylar
{

static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

Socket::ptr Socket::CreateTCP(Address::ptr addr)
{
    Socket::ptr sock(new Socket(addr->getFamily(), TCP, 0));
    return sock;
}
Socket::ptr Socket::CreateUDP(Address::ptr addr)
{
    Socket::ptr sock(new Socket(addr->getFamily(), UDP, 0));
    return sock;
}
Socket::ptr Socket::CreateTCPSocket()
{
    Socket::ptr sock(new Socket(IPv4, TCP, 0));
    return sock;
}
Socket::ptr Socket::CreateUDPSocket()
{
    Socket::ptr sock(new Socket(IPv4, UDP, 0));
    return sock;
}
Socket::ptr Socket::CreateTCPSocket6()
{
    Socket::ptr sock(new Socket(IPv6, TCP, 0));
    return sock;
}
Socket::ptr Socket::CreateUDPSocket6()
{
    Socket::ptr sock(new Socket(IPv6, UDP, 0));
    return sock;
}
Socket::ptr Socket::CreateUnixTCPSocket()
{
    Socket::ptr sock(new Socket(Unix, TCP, 0));
    return sock;
}

Socket::ptr Socket::CreateUnixUDPSocket()
{
    Socket::ptr sock(new Socket(Unix, UDP, 0));
    return sock;
}

Socket::Socket(int family, int type, int protocal)
    : m_sock(-1), m_family(family), m_type(type), m_protocol(protocal), m_isConnected(false)
{
}

Socket::~Socket()
{
    close();
}

int64_t Socket::getSendTimeOut() const
{
    FdCtx::ptr ctx = FdMgr::GetInstance()->get(m_sock);
    if (ctx)
    {
        return ctx->getTimeout(SO_SNDTIMEO);
    }
    return -1;
}

void Socket::setSendTimeOut(int64_t v)
{
    struct timeval tv = {int(v / 1000), int(v % 1000 * 1000)};
    setOption(SOL_SOCKET, SO_SNDTIMEO, tv);
}

int64_t Socket::getRecvTimeOut() const
{

    FdCtx::ptr ctx = FdMgr::GetInstance()->get(m_sock);
    if (ctx)
    {
        return ctx->getTimeout(SO_RCVTIMEO);
    }
    return -1;
}

void Socket::setRecvTimeOut(int64_t v)
{
    struct timeval tv
    {
        int(v / 1000), int(v % 1000 * 1000)
    };
    setOption(SOL_SOCKET, SO_RCVTIMEO, tv);
}

bool Socket::getOption(int level, int option, void *result, socklen_t *len)
{

    {
        int rt = getsockopt(m_sock, level, option, result, (socklen_t *)len);
        if (rt)
        {
            SYLAR_LOG_DEBUG(g_logger)
                << "getoption sock" << m_sock << " level = " << level << "option = " << option
                << " errno=" << errno << "errstr = " << strerror(errno);
        }
        return false;
    }
}

bool Socket::setOption(int level, int option, const void *result, socklen_t len)
{
    {
        if (setsockopt(m_sock, level, option, result, (socklen_t)len))
        {
            SYLAR_LOG_DEBUG(g_logger)
                << "setOption sock=" << m_sock << " level=" << level << " option=" << option
                << " errno=" << errno << " errstr=" << strerror(errno);
            return false;
        }
        return true;
    }
}

Socket::ptr Socket::accept()
{
    Socket::ptr sock(new Socket(m_family, m_type, m_protocol));
    int newsock = ::accept(m_sock, nullptr, nullptr);
    if (newsock == -1)
    {
        SYLAR_LOG_ERROR(g_logger) << "accept(" << m_sock << ") errno=" << errno
                                  << " errstr=" << strerror(errno);
        return nullptr;
    }
    if (sock->init(newsock))
    {
        //对象socket连接的一个socket，封装后返回的
        return sock;
    }
    return nullptr;
}

bool Socket::bind(const Address::ptr addr)
{
    if (!isValid())
    {
        newSock();
    }
    if (SYLAR_UNLIKELY(addr->getFamily() != m_family))
    {
        SYLAR_LOG_ERROR(g_logger) << "bind sock.family(" << m_family << ") addr.family("
                                  << addr->getFamily() << ") not equal, addr=" << addr->toString();
        return false;
    }

    UnixAddress::ptr uaddr = std::dynamic_pointer_cast<UnixAddress>(addr);
    if (uaddr)
    {
        Socket::ptr sock = Socket::CreateUnixTCPSocket();
        if (sock->connect(uaddr))
        {
            return false;
        }
        else
        {
            sylar::FSUtil::Unlink(uaddr->getPath(), true);
        }
    }

    if (::bind(m_sock, addr->getAddr(), addr->getAddrLen()))
    {
        SYLAR_LOG_ERROR(g_logger) << "bind error errrno=" << errno << " errstr=" << strerror(errno);
        return false;
    }
    getLocalAddress();
    return true;
}

bool Socket::connect(const Address::ptr addr, uint64_t timeout_ms)
{
    if (!isValid())
    {
        newSock();
        if (SYLAR_UNLIKELY(!isValid()))
        {
            return false;
        }
    }

    if (SYLAR_UNLIKELY(addr->getFamily() != m_family))
    {
        SYLAR_LOG_ERROR(g_logger) << "connect sock.family(" << m_family << ") addr.family("
                                  << addr->getFamily() << ") not equal, addr=" << addr->toString();
        return false;
    }

    if (timeout_ms == (uint64_t)-1)
    {
        if (::connect(m_sock, addr->getAddr(), addr->getAddrLen()))
        {
            SYLAR_LOG_ERROR(g_logger) << "sock=" << m_sock << " connect(" << addr->toString()
                                      << ") error errno=" << errno << " errstr=" << strerror(errno);
            close();
            return false;
        }
    }
    else
    {
        if (::connect_with_timeout(m_sock, addr->getAddr(), addr->getAddrLen(), timeout_ms))
        {
            SYLAR_LOG_ERROR(g_logger) << "sock=" << m_sock << " connect(" << addr->toString()
                                      << ") timeout=" << timeout_ms << " error errno=" << errno
                                      << " errstr=" << strerror(errno);
            close();
            return false;
        }
    }
    m_isConnected = true;
    getRemoteAddress();
    getLocalAddress();
    return true;
}

bool Socket::listen(int backlog)
{
    if (!isValid())
    {
        SYLAR_LOG_ERROR(g_logger) << "listen error sock=-1";
        return false;
    }
    if (::listen(m_sock, backlog))
    {
        SYLAR_LOG_ERROR(g_logger) << "listen error errno=" << errno
                                  << " errstr=" << strerror(errno);
        return false;
    }
    return true;
}

bool Socket::close()
{
    if (!m_isConnected && m_sock == -1)
    {
        return true;
    }
    m_isConnected = false;
    if (m_sock != -1)
    {
        ::close(m_sock);
        m_sock = -1;
    }
    return false;
}

int Socket::send(const void *buffer, size_t len, int flags)
{
    if (isConnected())
    {
        return ::send(m_sock, buffer, len, flags);
    }
    return -1;
}

int Socket::send(const iovec *buffer, size_t len, int flags)
{

    if (isConnected())
    {
        msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = (iovec *)buffer;
        msg.msg_iovlen = len;
        return ::sendmsg(m_sock, &msg, flags);
    }
    return -1;
}

int Socket::sendTo(const void *buffer, size_t len, const Address::ptr to, int flags)
{
    if (isConnected())
    {
        return ::sendto(m_sock, buffer, len, flags, to->getAddr(), to->getAddrLen());
    }
    return -1;
}

int Socket::sendTo(const iovec *buffer, size_t len, const Address::ptr to, int flags)
{
    if (isConnected())
    {
        msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = (iovec *)buffer;
        msg.msg_iovlen = len;
        msg.msg_name = to->getAddr();
        msg.msg_namelen = to->getAddrLen();
        return ::sendmsg(m_sock, &msg, flags);
    }
    return -1;
}

int Socket::recv(void *buffer, size_t len, int flags)
{
    if (isConnected())
    {
        return ::recv(m_sock, buffer, len, flags);
    }
    return -1;
}

int Socket::recv(iovec *buffer, size_t len, int flags)
{
    if (isConnected())
    {
        msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = (iovec *)buffer;
        msg.msg_iovlen = len;
        return ::recvmsg(m_sock, &msg, flags);
    }
    return -1;
}

int Socket::recvFrom(void *buffer, size_t len, Address::ptr from, int flags)
{
    if (isConnected())
    {
        socklen_t len = from->getAddrLen();
        return ::recvfrom(m_sock, buffer, len, flags, from->getAddr(), &len);
    }
    return -1;
}

int Socket::recvFrom(iovec *buffer, size_t len, Address::ptr from, int flags)
{
    if (isConnected())
    {
        msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = (iovec *)buffer;
        msg.msg_iovlen = len;
        msg.msg_name = from->getAddr();
        msg.msg_namelen = from->getAddrLen();
        return ::recvmsg(m_sock, &msg, flags);
    }
    return -1;
}

Address::ptr Socket::getLocalAddress()
{
    if (m_localAddress)
    {
        return m_localAddress;
    }
    Address::ptr result;
    switch (m_family)
    {
    case AF_INET:
        result.reset(new IPv4Address());
        break;
    case AF_INET6:
        result.reset(new IPv6Address());
        break;
    case AF_UNIX:
        result.reset(new UnixAddress());
        break;
    default:
        result.reset(new UnknownAddress(m_family));
        break;
    }
    socklen_t addrlen = result->getAddrLen();
    if (getsockname(m_sock, result->getAddr(), &addrlen))
    {
        SYLAR_LOG_ERROR(g_logger) << "getsockname error sock=" << m_sock << " errno=" << errno
                                  << " errstr=" << strerror(errno);
        return Address::ptr(new UnknownAddress(m_family));
    }
    if (m_family == AF_UNIX)
    {
        UnixAddress::ptr addr = std::dynamic_pointer_cast<UnixAddress>(result);
        addr->setAddrLen(addrlen);
    }
    m_localAddress = result;
    return m_localAddress;
}

Address::ptr Socket::getRemoteAddress()
{
    if (m_remoteAddress)
    {
        return m_remoteAddress;
    }

    Address::ptr result;
    switch (m_family)
    {
    case AF_INET:
        result.reset(new IPv4Address());
        break;
    case AF_INET6:
        result.reset(new IPv6Address());
        break;
    case AF_UNIX:
        result.reset(new UnixAddress());
        break;
    default:
        result.reset(new UnknownAddress(m_family));
        break;
    }
    socklen_t addrlen = result->getAddrLen();
    if (getpeername(m_sock, result->getAddr(), &addrlen))
    {
        SYLAR_LOG_ERROR(g_logger) << "getpeername error sock=" << m_sock << " errno=" << errno
                                  << " errstr=" << strerror(errno);
        return Address::ptr(new UnknownAddress(m_family));
    }
    if (m_family == AF_UNIX)
    {
        UnixAddress::ptr addr = std::dynamic_pointer_cast<UnixAddress>(result);
        addr->setAddrLen(addrlen);
    }
    m_remoteAddress = result;
    return m_remoteAddress;
}

bool Socket::isValid() const
{
    //只要不是空的一个我自己的封装的SOCKET，就认为是有效的
    return m_sock != -1;
}

int Socket::getError()
{
    int error = 0;
    socklen_t len = sizeof(error);
    if (!getOption(SOL_SOCKET, SO_ERROR, &error, &len))
    {
        return -1;
    }
    return error;
}
std::ostream &Socket::dump(std::ostream &os) const
{
    os << "[Socket sock=" << m_sock << " is_connected=" << m_isConnected << " family=" << m_family
       << " type=" << m_type << " protocol=" << m_protocol;
    if (m_localAddress)
    {
        os << " local_address=" << m_localAddress->toString();
    }
    if (m_remoteAddress)
    {
        os << " remote_address=" << m_remoteAddress->toString();
    }
    os << "]";
    return os;
}
bool Socket::cancelRead()
{
    return IOManager::GetThis()->cancelEvent(m_sock, sylar::IOManager::READ);
}

bool Socket::cancelWrite()
{
    return IOManager::GetThis()->cancelEvent(m_sock, sylar::IOManager::WRITE);
}

bool Socket::cancelAccept()
{
    return IOManager::GetThis()->cancelEvent(m_sock, sylar::IOManager::READ);
}

bool Socket::cancelAll()
{
    return IOManager::GetThis()->cancelAll(m_sock);
}

bool Socket::init(int sock)
{
    FdCtx::ptr ctx = FdMgr::GetInstance()->get(sock);
    if (ctx && !ctx->isClose() && ctx->isSocket())
    {
        m_sock = sock;
        m_isConnected = true;
        initSock();
        getLocalAddress();
        getRemoteAddress();
        return true;
    }
    return false;
}

void Socket::newSock()
{
    m_sock = socket(m_family, m_type, m_protocol);
    if (SYLAR_LIKELY(m_sock != -1))
    {
        FdMgr::GetInstance()->get(m_sock, true);
        initSock();
    }
    else
    {
        SYLAR_LOG_ERROR(g_logger) << "socket(" << m_family << ", " << m_type << ", " << m_protocol
                                  << ") errno=" << errno << " errstr=" << strerror(errno);
    }
}

void Socket::initSock()
{
    int val = 1;
    setOption(SOL_SOCKET, SO_REUSEADDR, val);
    if (m_type == SOCK_STREAM)
    {
        setOption(IPPROTO_TCP, TCP_NODELAY, val);
    }
}

namespace
{

struct _SSLInit
{
    _SSLInit()
    {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
    }
};

static _SSLInit s_init;

} // namespace

SSLSocket::SSLSocket(int family, int type, int protocol) : Socket(family, type, protocol)
{
}

Socket::ptr SSLSocket::accept()
{
    SSLSocket::ptr sock(new SSLSocket(m_family, m_type, m_protocol));
    int newsock = ::accept(m_sock, nullptr, nullptr);
    if (newsock == -1)
    {
        SYLAR_LOG_ERROR(g_logger) << "accept(" << m_sock << ") errno=" << errno
                                  << " errstr=" << strerror(errno);
        return nullptr;
    }
    sock->m_ctx = m_ctx;
    if (sock->init(newsock))
    {
        return sock;
    }
    return nullptr;
}

bool SSLSocket::bind(const Address::ptr addr)
{
    return Socket::bind(addr);
}

bool SSLSocket::connect(const Address::ptr addr, uint64_t timeout_ms)
{
    // 1. 先建立普通 TCP 连接
    // 这里调用父类 Socket::connect，负责 connect(fd, addr) 以及超时控制。
    bool v = Socket::connect(addr, timeout_ms);
    if (!v)
    {
        // TCP 都没连上，后面的 TLS 握手没有意义
        return false;
    }

    // 2. 创建 SSL_CTX
    // SSL_CTX 是 SSL/TLS 的上下文对象，可以理解为 TLS 配置环境。
    // TLS_client_method() 表示创建一个客户端 TLS 方法，支持根据环境协商 TLS 版本。
    //
    // m_ctx 是智能指针包装，第二个参数 SSL_CTX_free 是释放函数。
    m_ctx.reset(SSL_CTX_new(TLS_client_method()), SSL_CTX_free);
    if (!m_ctx)
    {
        SYLAR_LOG_ERROR(g_logger) << "SSL_CTX_new client context failed";
        return false;
    }

    // 3. 根据客户端配置决定是否验证服务端证书
    if (m_clientOptions.verify_peer)
    {
        // 开启服务端证书验证
        // SSL_VERIFY_PEER 表示握手过程中要验证对端证书链。
        SSL_CTX_set_verify(m_ctx.get(), SSL_VERIFY_PEER, nullptr);

        int loaded = 0;

        // 4. 加载 CA 信任库
        // 如果用户配置了 ca_file 或 ca_path，则使用用户指定的 CA 文件/目录。
        if (!m_clientOptions.ca_file.empty() || !m_clientOptions.ca_path.empty())
        {
            loaded = SSL_CTX_load_verify_locations(
                m_ctx.get(),
                m_clientOptions.ca_file.empty() ? nullptr : m_clientOptions.ca_file.c_str(),
                m_clientOptions.ca_path.empty() ? nullptr : m_clientOptions.ca_path.c_str());
        }
        else
        {
            // 如果用户没有指定 CA，就加载系统默认 CA 路径
            loaded = SSL_CTX_set_default_verify_paths(m_ctx.get());
        }

        // 加载 CA 失败，说明后续无法验证服务端证书链
        if (loaded != 1)
        {
            SYLAR_LOG_ERROR(g_logger) << "load TLS trust store failed";
            return false;
        }
    }
    else
    {
        // 不验证服务端证书
        // 这适合本地测试、自签证书调试，但不适合生产环境访问公网 HTTPS。
        SSL_CTX_set_verify(m_ctx.get(), SSL_VERIFY_NONE, nullptr);
    }

    // 5. 创建 SSL 会话对象
    // SSL_CTX 是配置环境，SSL 是一次具体连接上的 TLS 会话。
    m_ssl.reset(SSL_new(m_ctx.get()), SSL_free);

    // 6. 把 SSL 对象绑定到当前 socket fd
    // 之后 SSL_read / SSL_write / SSL_connect 都会通过 m_sock 这个 fd 通信。
    if (!m_ssl || SSL_set_fd(m_ssl.get(), m_sock) != 1)
    {
        SYLAR_LOG_ERROR(g_logger) << "initialize TLS session failed";
        return false;
    }

    // 7. 如果配置了 server_name，则设置 SNI 和主机名校验
    if (!m_clientOptions.server_name.empty())
    {
        // 7.1 设置 SNI
        // SNI 的作用是告诉服务端：我要访问哪个域名。
        // 同一个 IP 上可能部署多个 HTTPS 站点，服务端需要根据 SNI 返回正确证书。
        if (SSL_set_tlsext_host_name(m_ssl.get(), m_clientOptions.server_name.c_str()) != 1)
        {
            SYLAR_LOG_ERROR(g_logger) << "set TLS SNI failed host=" << m_clientOptions.server_name;
            return false;
        }

        // 7.2 设置主机名校验
        // verify_peer 只验证证书链是否可信；
        // X509_VERIFY_PARAM_set1_host 用来验证证书里的域名是否匹配 server_name。
        if (m_clientOptions.verify_peer &&
            X509_VERIFY_PARAM_set1_host(SSL_get0_param(m_ssl.get()),
                                        m_clientOptions.server_name.c_str(), 0) != 1)
        {
            SYLAR_LOG_ERROR(g_logger)
                << "set TLS hostname verification failed host=" << m_clientOptions.server_name;
            return false;
        }
    }

    // 8. 发起 TLS 握手
    // 这一步会进行 ClientHello、ServerHello、证书交换、密钥协商等流程。
    // 成功返回 1，失败返回 <= 0。
    if (SSL_connect(m_ssl.get()) != 1)
    {
        SYLAR_LOG_ERROR(g_logger) << "TLS handshake failed error="
                                  << ERR_error_string(ERR_get_error(), nullptr);
        return false;
    }

    // 9. 如果开启了证书验证，检查最终验证结果
    // X509_V_OK 表示证书链验证通过。
    if (m_clientOptions.verify_peer && SSL_get_verify_result(m_ssl.get()) != X509_V_OK)
    {
        SYLAR_LOG_ERROR(g_logger) << "TLS certificate verification failed result="
                                  << SSL_get_verify_result(m_ssl.get());
        return false;
    }

    // 10. TCP 连接成功 + TLS 握手成功 + 证书验证通过
    return true;
}

bool SSLSocket::listen(int backlog)
{
    return Socket::listen(backlog);
}

bool SSLSocket::close()
{
    return Socket::close();
}

int SSLSocket::send(const void *buffer, size_t length, int flags)
{
    if (m_ssl)
    {
        return SSL_write(m_ssl.get(), buffer, length);
    }
    return -1;
}

int SSLSocket::send(const iovec *buffers, size_t length, int flags)
{
    if (!m_ssl)
    {
        return -1;
    }
    int total = 0;
    for (size_t i = 0; i < length; ++i)
    {
        int tmp = SSL_write(m_ssl.get(), buffers[i].iov_base, buffers[i].iov_len);
        if (tmp <= 0)
        {
            return tmp;
        }
        total += tmp;
        if (tmp != (int)buffers[i].iov_len)
        {
            break;
        }
    }
    return total;
}

int SSLSocket::sendTo(const void *buffer, size_t length, const Address::ptr to, int flags)
{
    SYLAR_ASSERT(false);
    return -1;
}

int SSLSocket::sendTo(const iovec *buffers, size_t length, const Address::ptr to, int flags)
{
    SYLAR_ASSERT(false);
    return -1;
}

int SSLSocket::recv(void *buffer, size_t length, int flags)
{
    if (m_ssl)
    {
        return SSL_read(m_ssl.get(), buffer, length);
    }
    return -1;
}

int SSLSocket::recv(iovec *buffers, size_t length, int flags)
{
    if (!m_ssl)
    {
        return -1;
    }
    int total = 0;
    for (size_t i = 0; i < length; ++i)
    {
        int tmp = SSL_read(m_ssl.get(), buffers[i].iov_base, buffers[i].iov_len);
        if (tmp <= 0)
        {
            return tmp;
        }
        total += tmp;
        if (tmp != (int)buffers[i].iov_len)
        {
            break;
        }
    }
    return total;
}

int SSLSocket::recvFrom(void *buffer, size_t length, Address::ptr from, int flags)
{
    SYLAR_ASSERT(false);
    return -1;
}

int SSLSocket::recvFrom(iovec *buffers, size_t length, Address::ptr from, int flags)
{
    SYLAR_ASSERT(false);
    return -1;
}

bool SSLSocket::init(int sock)
{
    bool v = Socket::init(sock);
    if (v)
    {
        m_ssl.reset(SSL_new(m_ctx.get()), SSL_free);
        SSL_set_fd(m_ssl.get(), m_sock);
        v = (SSL_accept(m_ssl.get()) == 1);
    }
    return v;
}

bool SSLSocket::loadCertificates(const std::string &cert_file, const std::string &key_file)
{
    m_ctx.reset(SSL_CTX_new(SSLv23_server_method()), SSL_CTX_free);
    if (SSL_CTX_use_certificate_chain_file(m_ctx.get(), cert_file.c_str()) != 1)
    {
        SYLAR_LOG_ERROR(g_logger) << "SSL_CTX_use_certificate_chain_file(" << cert_file
                                  << ") error";
        return false;
    }
    if (SSL_CTX_use_PrivateKey_file(m_ctx.get(), key_file.c_str(), SSL_FILETYPE_PEM) != 1)
    {
        SYLAR_LOG_ERROR(g_logger) << "SSL_CTX_use_PrivateKey_file(" << key_file << ") error";
        return false;
    }
    if (SSL_CTX_check_private_key(m_ctx.get()) != 1)
    {
        SYLAR_LOG_ERROR(g_logger) << "SSL_CTX_check_private_key cert_file=" << cert_file
                                  << " key_file=" << key_file;
        return false;
    }
    return true;
}

SSLSocket::ptr SSLSocket::CreateTCP(sylar::Address::ptr address)
{
    SSLSocket::ptr sock(new SSLSocket(address->getFamily(), TCP, 0));
    return sock;
}

SSLSocket::ptr SSLSocket::CreateTCPSocket()
{
    SSLSocket::ptr sock(new SSLSocket(IPv4, TCP, 0));
    return sock;
}

SSLSocket::ptr SSLSocket::CreateTCPSocket6()
{
    SSLSocket::ptr sock(new SSLSocket(IPv6, TCP, 0));
    return sock;
}

std::ostream &SSLSocket::dump(std::ostream &os) const
{
    os << "[SSLSocket sock=" << m_sock << " is_connected=" << m_isConnected
       << " family=" << m_family << " type=" << m_type << " protocol=" << m_protocol;
    if (m_localAddress)
    {
        os << " local_address=" << m_localAddress->toString();
    }
    if (m_remoteAddress)
    {
        os << " remote_address=" << m_remoteAddress->toString();
    }
    os << "]";
    return os;
}

std::ostream &operator<<(std::ostream &os, const Socket &sock)
{
    return sock.dump(os);
}

} // namespace sylar
