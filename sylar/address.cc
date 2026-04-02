#include "address.h"
#include "endian.h"
#include "log.h"
#include <cstddef>
#include <ifaddrs.h>
#include <netdb.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/types.h>

namespace sylar
{

static sylar::Logger::ptr g_logger = SYLAR_LOG_ROOT();

template <class T>
static T CreateMask(uint32_t bits)
{
    //如果前缀bits是24，那么主机位掩码就是下面这个
    // 0000 0000 0000 0000 0000 0000 1111 1111
    return (1 << (sizeof(T) * 8 - bits)) - 1;
}

// 计算子网掩码里面的前缀1的个数
template <class T>
static uint32_t CountBytes(T value)
{
    uint32_t result = 0;
    for (; value; ++result)
    {
        value &= value - 1;
    }
    return result;
}

/**
 * @brief 查找地址(解析域名,ip地址)
 *
 * @param result 结果
 * @param host 主机名
    下面的都是希望拿到的该域名下的符合这些条件的具体的地址
 * @param family 地址族
 * @param type 套接字类型
 * @param protocol 协议
 * @return true 成功
 * @return false 失败
 */
bool Address::Lookup(
    std::vector<Address::ptr> &result, const std::string &host, int family, int type, int protocol)
{
    addrinfo hints, *results, *next;
    hints.ai_flags = 0;
    hints.ai_family = family;
    hints.ai_socktype = type;
    hints.ai_protocol = protocol;
    hints.ai_addrlen = 0;
    hints.ai_canonname = NULL;
    hints.ai_addr = NULL;
    hints.ai_next = NULL;

    //主机地址
    std::string node;
    // 端口或服务名
    const char *service = NULL;

    //检查 ipv6address serivce
    // [2001:db8::1]:80，ipv6的格式是这样的，所以下面的就是判断 是 ipv6地址
    if (!host.empty() && host[0] == '[')
    {
        //从 '[' 后面开始找 ']',如果找到了，说明地址部分结束。
        const char *endipv6 = (const char *)memchr(host.c_str() + 1, ']', host.size() - 1);
        if (endipv6)
        {
            if (*(endipv6 + 1) == ':')
            {
                service = endipv6 + 2;
            }
            node = host.substr(1, endipv6 - host.c_str() - 1);
        }
    }

    //检查 node serivce
    // 如果前面没按 [IPv6]:port 解析出来，
    // 那再尝试普通的： host:port 格式,也就是看是不是 ipv4的，ipv4只有一个冒号
    if (node.empty())
    {
        service = (const char *)memchr(host.c_str(), ':', host.size());
        if (service)
        {
            // 从找到的第一个冒号继续往后找，一直找到末尾
            const char *begin = service + 1;
            const char *end = host.c_str() + host.size();
            size_t len = end - begin;

            if (!memchr(begin, ':', len))
            {
                // 后面没有 ':'，那就是ipv4的格式
                node = host.substr(0, service - host.c_str());
                ++service;
            }
        }
    }

    if (node.empty())
    {
        node = host;
    }
    //不是[]这样的ipv6，也不是ipv4，只要不是那种完全错的，那就只剩下标准的格式了，那就交给
    // getaddrinfo 去解析
    int error = getaddrinfo(node.c_str(), service, &hints, &results);
    if (error)
    {
        SYLAR_LOG_ERROR(g_logger) << "Address::Lookup getaddress(" << host << ", " << family << ", "
                                  << type << ") err=" << error << " errstr=" << gai_strerror(error);
        return false;
    }

    next = results;
    while (next)
    {
        result.push_back(Create(next->ai_addr, (socklen_t)next->ai_addrlen));
        // SYLAR_LOG_INFO(g_logger) << ((sockaddr_in*)next->ai_addr)->sin_addr.s_addr;
        next = next->ai_next;
    }

    freeaddrinfo(results);
    return true;
}

Address::ptr Address::LookupAny(const std::string &host, int family, int type, int protocol)
{
    std::vector<Address::ptr> result;
    if (Lookup(result, host, family, type, protocol))
    {
        return result[0];
    }
    return nullptr;
}

IPAddress::ptr
Address::LookupAnyIPAddress(const std::string &host, int family, int type, int protocol)
{
    std::vector<Address::ptr> result;
    if (Lookup(result, host, family, type, protocol))
    {
        // for (auto &i : result)
        // {
        //     std::cout << i->toString() << std::endl;
        // }
        for (auto &i : result)
        {
            IPAddress::ptr v = std::dynamic_pointer_cast<IPAddress>(i);
            if (v)
            {
                return v;
            }
        }
    }
    return nullptr;
}

bool Address::GetInterfaceAddresses(
    std::multimap<std::string, std::pair<Address::ptr, uint32_t>> &result, int family)
{
    struct ifaddrs *next, *results;
    // 拿到本机的所有网卡的地址，一个网卡名对应着多个地址，放到results里
    if (getifaddrs(&results) != 0)
    {
        SYLAR_LOG_ERROR(g_logger) << "Address::GetInterfaceAddresses getifaddrs "
                                     " err="
                                  << errno << " errstr=" << strerror(errno);
        return false;
    }

    try
    {
        for (next = results; next; next = next->ifa_next)
        {
            Address::ptr addr;
            uint32_t prefix_len = ~0u;
            if (family != AF_UNSPEC && family != next->ifa_addr->sa_family)
            {
                continue;
            }
            switch (next->ifa_addr->sa_family)
            {
            case AF_INET:
            {
                addr = Create(next->ifa_addr, sizeof(sockaddr_in));
                //拿到子网掩码
                uint32_t netmask = ((sockaddr_in *)next->ifa_netmask)->sin_addr.s_addr;
                prefix_len = CountBytes(netmask);
            }
            break;
            case AF_INET6:
            {
                addr = Create(next->ifa_addr, sizeof(sockaddr_in6));
                in6_addr &netmask = ((sockaddr_in6 *)next->ifa_netmask)->sin6_addr;
                prefix_len = 0;
                for (int i = 0; i < 16; ++i)
                {
                    prefix_len += CountBytes(netmask.s6_addr[i]);
                }
            }
            break;
            default:
                break;
            }

            if (addr)
            {
                result.insert(std::make_pair(next->ifa_name, std::make_pair(addr, prefix_len)));
            }
        }
    }
    catch (...)
    {
        SYLAR_LOG_ERROR(g_logger) << "Address::GetInterfaceAddresses exception";
        freeifaddrs(results);
        return false;
    }
    freeifaddrs(results);
    return true;
}
bool Address::GetInterfaceAddresses(std::vector<std::pair<Address::ptr, uint32_t>> &result,
                                    const std::string &iface,
                                    int family)
{
    //这里返回的根本不是具体网卡地址，更像“通配绑定地址”
    if (iface.empty() || iface == "*")
    {
        if (family == AF_INET || family == AF_UNSPEC)
        {
            result.push_back(std::make_pair(Address::ptr(new IPv4Address()), 0u));
        }
        if (family == AF_INET6 || family == AF_UNSPEC)
        {
            result.push_back(std::make_pair(Address::ptr(new IPv6Address()), 0u));
        }
        return true;
    }

    std::multimap<std::string, std::pair<Address::ptr, uint32_t>> results;

    if (!GetInterfaceAddresses(results, family))
    {
        return false;
    }

    // 对于 multimap，同一个 key 可能有多个值。
    // equal_range(iface) 返回一段区间，表示所有 key 等于iface 的项。
    auto its = results.equal_range(iface);
    // its.first：指向 第一个 key == iface 的元素
    // its.second：指向 最后一个 key == iface 的元素的下一个位置
    for (; its.first != its.second; ++its.first)
    {
        result.push_back(its.first->second);
    }
    return true;
}

int Address::getFamily() const
{
    return getAddr()->sa_family;
}

std::string Address::toString()
{
    std::stringstream ss;
    insert(ss);
    return ss.str();
}

std::string Address::toString() const
{
    std::stringstream ss;
    insert(ss);
    return ss.str();
}

Address::ptr Address::Create(const sockaddr *addr, socklen_t addrlen)
{
    if (addr == nullptr)
    {
        return nullptr;
    }

    Address::ptr result;
    switch (addr->sa_family)
    {
    case AF_INET:
        result.reset(new IPv4Address(*(const sockaddr_in *)addr));
        break;
    case AF_INET6:
        result.reset(new IPv6Address(*(const sockaddr_in6 *)addr));
        break;
    default:
        result.reset(new UnknownAddress(*addr));
        break;
    }
    return result;
}

bool Address::operator<(const Address &rhs) const
{
    socklen_t minlen = std::min(getAddrLen(), rhs.getAddrLen());
    //比较地址的前n个字节，<0 代表 ptr1<ptr2
    int result = memcmp(getAddr(), rhs.getAddr(), minlen);

    //返回true和false,实际代表的是a应该在b的前面吗，true代表a应该在b的前面
    if (result < 0)
    {
        return true;
    }
    else if (result > 0)
    {
        return false;
    }
    else if (getAddrLen() < rhs.getAddrLen())
    {
        return true;
    }
    return false;
}

bool Address::operator==(const Address &rhs) const
{
    return getAddrLen() == rhs.getAddrLen() && memcmp(getAddr(), rhs.getAddr(), getAddrLen()) == 0;
}

bool Address::operator!=(const Address &rhs) const
{
    return !(*this == rhs);
}

IPAddress::ptr IPAddress::Create(const char *address, uint16_t port)
{
    addrinfo hints, *results;
    memset(&hints, 0, sizeof(addrinfo));

    hints.ai_flags = AI_NUMERICHOST;
    hints.ai_family = AF_UNSPEC;

    int error = getaddrinfo(address, NULL, &hints, &results);
    if (error)
    {
        SYLAR_LOG_ERROR(g_logger) << "IPAddress::Create(" << address << ", " << port
                                  << ") error=" << error << " errstr=" << gai_strerror(error);
        return nullptr;
    }

    try
    {
        IPAddress::ptr result = std::dynamic_pointer_cast<IPAddress>(
            Address::Create(results->ai_addr, (socklen_t)results->ai_addrlen));
        if (result)
        {
            result->setPort(port);
        }
        freeaddrinfo(results);
        return result;
    }
    catch (...)
    {
        freeaddrinfo(results);
        return nullptr;
    }
}

IPv4Address::ptr IPv4Address::Create(const char *address, uint16_t port)
{
    IPv4Address::ptr rt(new IPv4Address);
    rt->m_addr.sin_port = byteswapOnLittleEndian(port);
    int result = inet_pton(AF_INET, address, &rt->m_addr.sin_addr);
    if (result <= 0)
    {
        SYLAR_LOG_ERROR(g_logger) << "IPv4Address::Create(" << address << ", " << port
                                  << ") rt=" << result << " errno=" << errno
                                  << " errstr=" << strerror(errno);
        return nullptr;
    }
    return rt;
}

IPv4Address::IPv4Address(const sockaddr_in &address)
{
    m_addr = address;
}

IPv4Address::IPv4Address(uint32_t address, uint16_t port)
{
    memset(&m_addr, 0, sizeof(m_addr));
    m_addr.sin_family = AF_INET;
    m_addr.sin_port = byteswapOnLittleEndian(port);
    m_addr.sin_addr.s_addr = byteswapOnLittleEndian(address);
}

const sockaddr *IPv4Address::getAddr() const
{
    return (sockaddr *)&m_addr;
}

sockaddr *IPv4Address::getAddr()
{
    return (sockaddr *)&m_addr;
}

socklen_t IPv4Address::getAddrLen() const
{
    return sizeof(m_addr);
}

std::ostream &IPv4Address::insert(std::ostream &os) const
{
    char buf[INET_ADDRSTRLEN] = {0};

    // 把 IPv4 地址转换成点分十进制字符串
    // INET_ADDRSTRLEN 对 IPv4 足够，通常是 16
    const char *result = inet_ntop(AF_INET, &m_addr.sin_addr, buf, INET_ADDRSTRLEN);

    if (result)
    {
        os << buf;
    }
    else
    {
        // 转换失败时给个兜底输出，避免流内容异常
        os << "[invalid IPv4]";
    }

    // sin_port 在 sockaddr_in 里是网络字节序
    // 输出前要转回主机字节序
    os << ":" << byteswapOnLittleEndian(m_addr.sin_port);
    return os;
}

IPAddress::ptr IPv4Address::broadcastAddress(uint32_t prefix_len)
{
    if (prefix_len > 32)
    {
        return nullptr;
    }

    sockaddr_in baddr(m_addr);
    baddr.sin_addr.s_addr |= byteswapOnLittleEndian(CreateMask<uint32_t>(prefix_len));
    return IPv4Address::ptr(new IPv4Address(baddr));
}

IPAddress::ptr IPv4Address::networkAddress(uint32_t prefix_len)
{
    if (prefix_len > 32)
    {
        return nullptr;
    }

    sockaddr_in baddr(m_addr);
    //这里的~运算是我自己加的，sylar没加
    baddr.sin_addr.s_addr &= ~byteswapOnLittleEndian(CreateMask<uint32_t>(prefix_len));
    return IPv4Address::ptr(new IPv4Address(baddr));
}

IPAddress::ptr IPv4Address::subnetMask(uint32_t prefix_len)
{
    sockaddr_in subnet;
    memset(&subnet, 0, sizeof(subnet));
    subnet.sin_family = AF_INET;
    subnet.sin_addr.s_addr = ~byteswapOnLittleEndian(CreateMask<uint32_t>(prefix_len));
    return IPv4Address::ptr(new IPv4Address(subnet));
}

uint16_t IPv4Address::getPort() const
{
    return byteswapOnLittleEndian(m_addr.sin_port);
}

void IPv4Address::setPort(uint16_t v)
{
    m_addr.sin_port = byteswapOnLittleEndian(v);
}

IPv6Address::ptr IPv6Address::Create(const char *address, uint16_t port)
{
    IPv6Address::ptr rt(new IPv6Address);
    rt->m_addr.sin6_port = byteswapOnLittleEndian(port);
    int result = inet_pton(AF_INET6, address, &rt->m_addr.sin6_addr);
    if (result <= 0)
    {
        SYLAR_LOG_ERROR(g_logger) << "IPv6Address::Create(" << address << ", " << port
                                  << ") rt=" << result << " errno=" << errno
                                  << " errstr=" << strerror(errno);
        return nullptr;
    }
    return rt;
}

IPv6Address::IPv6Address()
{
    memset(&m_addr, 0, sizeof(m_addr));
    m_addr.sin6_family = AF_INET6;
}

IPv6Address::IPv6Address(const sockaddr_in6 &address)
{
    m_addr = address;
}

// sin6_addr.s6_addr本身就是一个长度为 16 的字节数组，所以直接拷贝过去就行。
IPv6Address::IPv6Address(const uint8_t address[16], uint16_t port)
{
    memset(&m_addr, 0, sizeof(m_addr));
    m_addr.sin6_family = AF_INET6;
    m_addr.sin6_port = byteswapOnLittleEndian(port);
    memcpy(m_addr.sin6_addr.s6_addr, address, 16);
}

const sockaddr *IPv6Address::getAddr() const
{
    return (sockaddr *)&m_addr;
}

sockaddr *IPv6Address::getAddr()
{
    return (sockaddr *)&m_addr;
}

socklen_t IPv6Address::getAddrLen() const
{
    return sizeof(m_addr);
}

std::ostream &IPv6Address::insert(std::ostream &os) const
{
    char buf[INET6_ADDRSTRLEN] = {0};

    // 把二进制 IPv6 地址转成标准文本形式
    const char *ret = inet_ntop(AF_INET6, &m_addr.sin6_addr, buf, sizeof(buf));
    if (!ret)
    {
        // 转换失败时给一个明确输出，避免流内容不完整
        return os << "[invalid IPv6 address]"
                  << ":" << ntohs(m_addr.sin6_port);
    }

    os << "[" << buf << "]:" << ntohs(m_addr.sin6_port);
    return os;
}

IPAddress::ptr IPv6Address::broadcastAddress(uint32_t prefix_len)
{
    sockaddr_in6 baddr(m_addr);
    // 把一个字节里面属于主机位的地方置为1
    baddr.sin6_addr.s6_addr[prefix_len / 8] |= CreateMask<uint8_t>(prefix_len % 8);
    // 把那个那个字节后面的主机位全部置为1
    for (int i = prefix_len / 8 + 1; i < 16; ++i)
    {
        baddr.sin6_addr.s6_addr[i] = 0xff;
    }
    return IPv6Address::ptr(new IPv6Address(baddr));
}

IPAddress::ptr IPv6Address::networkAddress(uint32_t prefix_len)
{
    sockaddr_in6 baddr(m_addr);
    baddr.sin6_addr.s6_addr[prefix_len / 8] &= ~CreateMask<uint8_t>(prefix_len % 8);
    for (int i = prefix_len / 8 + 1; i < 16; ++i)
    {
        baddr.sin6_addr.s6_addr[i] = 0x00;
    }
    return IPv6Address::ptr(new IPv6Address(baddr));
}

IPAddress::ptr IPv6Address::subnetMask(uint32_t prefix_len)
{
    sockaddr_in6 subnet;
    memset(&subnet, 0, sizeof(subnet));
    subnet.sin6_family = AF_INET6;
    subnet.sin6_addr.s6_addr[prefix_len / 8] = ~CreateMask<uint8_t>(prefix_len % 8);

    for (uint32_t i = 0; i < prefix_len / 8; ++i)
    {
        subnet.sin6_addr.s6_addr[i] = 0xff;
    }
    return IPv6Address::ptr(new IPv6Address(subnet));
}

uint16_t IPv6Address::getPort() const
{
    return byteswapOnLittleEndian(m_addr.sin6_port);
}

void IPv6Address::setPort(uint16_t v)
{
    m_addr.sin6_port = byteswapOnLittleEndian(v);
}

//为什么减 1
// 因为通常路径字符串要留一个位置给结尾的 '\0'。
static const size_t MAX_PATH_LEN = sizeof(((sockaddr_un *)0)->sun_path) - 1;
UnixAddress::UnixAddress()
{
    memset(&m_addr, 0, sizeof(m_addr));
    m_addr.sun_family = AF_UNIX;
    m_length = offsetof(sockaddr_un, sun_path) + MAX_PATH_LEN;
}

UnixAddress::UnixAddress(const std::string &path)
{
    memset(&m_addr, 0, sizeof(m_addr));
    m_addr.sun_family = AF_UNIX;
    m_length = path.size() + 1;

    if (!path.empty() && path[0] == '\0')
    {
        --m_length;
    }

    if (m_length > sizeof(m_addr.sun_path))
    {
        throw std::logic_error("path too long");
    }
    memcpy(m_addr.sun_path, path.c_str(), m_length);
    m_length += offsetof(sockaddr_un, sun_path);
}

const sockaddr *UnixAddress::getAddr() const
{
    return (sockaddr *)&m_addr;
}
sockaddr *UnixAddress::getAddr()
{
    return (sockaddr *)&m_addr;
}

socklen_t UnixAddress::getAddrLen() const
{
    return m_length;
}

void UnixAddress::setAddrLen(uint32_t v)
{
    m_length = v;
}

std::ostream &UnixAddress::insert(std::ostream &os) const
{
    // sockaddr_un 中 sun_path 字段起始偏移
    const size_t path_offset = offsetof(sockaddr_un, sun_path);

    // 长度异常：连 sun_path 都还没覆盖到
    if (m_length <= path_offset)
    {
        return os << "[invalid unix address]";
    }

    // sun_path 的有效字节数
    const size_t path_len = m_length - path_offset;

    // Linux 抽象命名空间地址：
    // 第一个字节为 '\0'，后面的字节才是名字，且不要求以 '\0' 结尾
    if (m_addr.sun_path[0] == '\0')
    {
        os << "\\0";
        return os.write(m_addr.sun_path + 1, path_len - 1);
    }

    // 普通文件系统路径：
    // 不能直接把 sun_path 当 C 字符串输出，应该按长度处理
    // 但如果最后一个字节正好是 '\0'，通常不希望把它输出出来
    size_t real_len = path_len;
    if (real_len > 0 && m_addr.sun_path[real_len - 1] == '\0')
    {
        --real_len;
    }

    return os.write(m_addr.sun_path, real_len);
}

UnknownAddress::UnknownAddress(int family)
{
    memset(&m_addr, 0, sizeof(m_addr));
    m_addr.sa_family = family;
}

UnknownAddress::UnknownAddress(const sockaddr &addr)
{
    m_addr = addr;
}

const sockaddr *UnknownAddress::getAddr() const
{
    return &m_addr;
}
sockaddr *UnknownAddress::getAddr()
{
    return &m_addr;
}

socklen_t UnknownAddress::getAddrLen() const
{
    return sizeof(m_addr);
}

std::ostream &UnknownAddress::insert(std::ostream &os) const
{
    os << "[UnknownAddress family=" << m_addr.sa_family << "]";
    return os;
}
} // namespace sylar
