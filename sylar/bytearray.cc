#include "bytearray.h"
#include "endian.h"
#include "log.h"
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <memory>
#include <stdexcept>
#include <string>
namespace sylar
{
static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

ByteArray::Node::Node(size_t s) : ptr(new char[s]), next(nullptr), size(s)
{
}

ByteArray::Node::Node() : ptr(nullptr), next(nullptr), size(0)
{
}

ByteArray::Node::~Node()
{
    if (ptr)
        delete[] ptr;
}

ByteArray::ByteArray(size_t base_size)
    : m_baseSize(base_size), m_position(0), m_capacity(base_size), m_size(0),
      m_endian(SYLAR_BIG_INDIAN), m_root(new Node(base_size)), m_cur(m_root)
{
}

ByteArray::~ByteArray()
{
    Node *tmp = m_root;
    while (tmp)
    {
        m_cur = tmp;
        tmp = tmp->next;
        delete m_cur;
    }
}

bool ByteArray::isLittleIndian()
{
    return m_endian == SYLAR_LITTLE_INDIAN;
}

void ByteArray::setLittleIndian(bool val)
{
    if (val)
    {
        m_endian = SYLAR_LITTLE_INDIAN;
    }
    else
    {
        m_endian = SYLAR_BIG_INDIAN;
    }
}

void ByteArray::writeFint8(int8_t value)
{
    write(&value, sizeof(value));
}

void ByteArray::writeFuint8(uint8_t value)
{
    write(&value, sizeof(value));
}

void ByteArray::writeFint16(int16_t value)
{
    if (m_endian != SYLAR_BYTE_ORDER)
    {
        value = byteswap(value);
    }
    write(&value, sizeof(value));
}

void ByteArray::writeFuint16(uint16_t value)
{
    if (m_endian != SYLAR_BYTE_ORDER)
    {
        value = byteswap(value);
    }
    write(&value, sizeof(value));
}

void ByteArray::writeFint32(int32_t value)
{
    if (m_endian != SYLAR_BYTE_ORDER)
    {
        value = byteswap(value);
    }
    write(&value, sizeof(value));
}

void ByteArray::writeFuint32(uint32_t value)
{
    if (m_endian != SYLAR_BYTE_ORDER)
    {
        value = byteswap(value);
    }
    write(&value, sizeof(value));
}

void ByteArray::writeFint64(int64_t value)
{
    if (m_endian != SYLAR_BYTE_ORDER)
    {
        value = byteswap(value);
    }
    write(&value, sizeof(value));
}

void ByteArray::writeFuint64(uint64_t value)
{
    if (m_endian != SYLAR_BYTE_ORDER)
    {
        value = byteswap(value);
    }
    write(&value, sizeof(value));
}

// zig-zag 编码,负数取绝对值的*2-1，正数取绝对值的*2
static uint32_t EncodeZigzag32(const int32_t &value)
{
    return (value << 1) ^ (value >> 31);
}

static uint64_t EncodeZigzag64(const int64_t &value)
{
    return (value << 1) ^ (value >> 63);
}

static int32_t DecodeZigzag32(const uint32_t &value)
{
    return (value >> 1) ^ -(value & 1);
}

static int64_t DecodeZigzag64(const uint64_t &value)
{
    return (value >> 1) ^ -(value & 1);
}

void ByteArray::writeInt32(int32_t value)
{
    writeUint32(EncodeZigzag32(value));
}

// varint 编码
void ByteArray::writeUint32(uint32_t value)
{
    uint8_t tmp[5];
    uint8_t i = 0;
    //对于uint，每8位，最高位为标志位，其他位为数据位
    //如果最高位为1，说明还有更多数据位
    //如果value>0x80(也就是128),也就是说最高位为1，也就是还有更多数据位
    while (value >= 0x80)
    {
        //每次取出来数据位，连到tmp上
        //（value & 0x7f）取出来低7位
        // （|0x80）表示设置最高位为1
        tmp[i++] = (value & 0x7f) | 0x80;
        //后面的八位取完了，可以取更往前的八位了
        value >>= 7;
    }
    //把最后的8位也连到tmp上
    tmp[i++] = value;
    write(tmp, i);
}

void ByteArray::writeInt64(int64_t value)
{
    writeUint64(EncodeZigzag64(value));
}

void ByteArray::writeUint64(uint64_t value)
{
    uint8_t tmp[10];
    uint8_t i = 0;
    while (value >= 0x80)
    {
        tmp[i++] = (value & 0x7F) | 0x80;
        value >>= 7;
    }
    tmp[i++] = value;
    write(tmp, i);
}

void ByteArray::writeFloat(float value)
{
    //转一下，是因为byteswap(float)也可以直接转，但是毕竟是float，语义不太清楚
    //然后转完之后语义更清楚了,也可以复用之前的逻辑
    uint32_t v;
    memcpy(&v, &value, sizeof(value));
    writeFuint32(v);
}

void ByteArray::writeDouble(double value)
{
    uint64_t v;
    memcpy(&v, &value, sizeof(value));
    writeFuint64(v);
}

void ByteArray::writeStringF16(const std::string &value)
{
    //这样写是因为读的时候，要先读取长度，再读取字符串
    writeFuint16(value.size());
    write(value.c_str(), value.size());
}

void ByteArray::writeStringF32(const std::string &value)
{
    writeFuint32(value.size());
    write(value.c_str(), value.size());
}

void ByteArray::writeStringF64(const std::string &value)
{
    writeFuint64(value.size());
    write(value.c_str(), value.size());
}

void ByteArray::writeStringVint(const std::string &value)
{
    writeUint64(value.size());
    write(value.c_str(), value.size());
}

void ByteArray::writeStringWithoutLength(const std::string &value)
{
    write(value.c_str(), value.size());
}

int8_t ByteArray::readFint8()
{
    int8_t v;
    read(&v, sizeof(v));
    return v;
}

uint8_t ByteArray::readFuint8()
{
    uint8_t v;
    read(&v, sizeof(v));
    return v;
}

#define XX(type)                                                                                   \
    type v;                                                                                        \
    read(&v, sizeof(v));                                                                           \
    if (m_endian == SYLAR_BYTE_ORDER)                                                              \
    {                                                                                              \
        return v;                                                                                  \
    }                                                                                              \
    else                                                                                           \
    {                                                                                              \
        return byteswap(v);                                                                        \
    }

int16_t ByteArray::readFint16()
{
    XX(int16_t);
}
uint16_t ByteArray::readFuint16()
{
    XX(uint16_t);
}

int32_t ByteArray::readFint32()
{
    XX(int32_t);
}

uint32_t ByteArray::readFuint32()
{
    XX(uint32_t);
}

int64_t ByteArray::readFint64()
{
    XX(int64_t);
}

uint64_t ByteArray::readFuint64()
{
    XX(uint64_t);
}

#undef XX

int32_t ByteArray::readInt32()
{
    //先读到该读的字节，然后把字节转换为int32_t
    return DecodeZigzag32(readUint32());
}

uint32_t ByteArray::readUint32()
{
    uint32_t result = 0;
    //用循环是为了防止wirte进去的有错，读出来的也有错
    for (int i = 0; i < 32; i += 7)
    {
        //因为wirteUint32是先写进去的低位，那么这里读取的时候就是先读到的低位
        uint8_t b = readFuint8();
        //读到最后了，放到最前面
        if (b < 0x80)
        {
            result |= ((uint32_t)b) << i;
            break;
        }
        //先读到的是最低位的
        else
        {
            result |= (((uint32_t)(b & 0x7f)) << i);
        }
    }
    return result;
}

int64_t ByteArray::readInt64()
{
    return DecodeZigzag64(readUint64());
}

uint64_t ByteArray::readUint64()
{
    uint64_t result = 0;
    for (int i = 0; i < 64; i += 7)
    {
        uint8_t b = readFuint8();
        if (b < 0x80)
        {
            result |= ((uint64_t)b) << i;
            break;
        }
        else
        {
            result |= (((uint64_t)(b & 0x7f)) << i);
        }
    }
    return result;
}

float ByteArray::readFloat()
{
    uint32_t v = readFuint32();
    float result;
    memcpy(&result, &v, sizeof(v));
    return result;
}

double ByteArray::readDouble()
{
    uint64_t v = readFuint64();
    double value;
    memcpy(&value, &v, sizeof(v));
    return value;
}

std::string ByteArray::readStringF16()
{
    //先读到长度，这个长度就代表后面的字符串实际上是多长
    uint16_t len = readFuint16();
    std::string buffer;
    buffer.resize(len);
    // buffer是一个string对象，里面是有其他属性的，然后string里面有一个指针，指向的
    // 是实际上的数据存储的地址，我们要写入的就是那个地址，而不是要覆盖掉string这个对象本身
    read(&buffer[0], len);
    return buffer;
}

std::string ByteArray::readStringF32()
{
    uint32_t len = readFuint32();
    std::string buff;
    buff.resize(len);
    read(&buff[0], len);
    return buff;
}

std::string ByteArray::readStringF64()
{
    uint64_t len = readFuint64();
    std::string buff;
    buff.resize(len);
    read(&buff[0], len);
    return buff;
}

std::string ByteArray::readStringVint()
{
    uint64_t len = readUint64();
    std::string buff;
    buff.resize(len);
    read(&buff[0], len);
    return buff;
}

void ByteArray::clear()
{
    m_position = m_size = 0;
    m_capacity = m_baseSize;
    //清空root后面连接起来的
    Node *tmp = m_root->next;
    while (tmp)
    {
        m_cur = tmp;
        tmp = tmp->next;
        delete m_cur;
    }
    m_cur = m_root;
    m_root->next = nullptr;
}

void ByteArray::write(const void *buf, size_t size)
{
    if (size == 0)
    {
        return;
    }
    //确保空间足够
    addCapacity(size);
    //计算写入的位置(块内偏移)
    size_t nowPos = m_position % m_baseSize;
    //当前块还有多少空间可写
    size_t nowCap = m_cur->size - nowPos;
    // bufPos表示buf已经写入了多少
    size_t bufPos = 0;

    while (size > 0)
    {
        //如果当前的块还能放下剩下的所有数据
        if (nowCap >= size)
        {
            memcpy(m_cur->ptr + nowPos, (const char *)buf + bufPos, size);
            if (m_cur->size == (nowPos + size))
            {
                //如果当前的块已经写满了，那么就切换到下一个块
                m_cur = m_cur->next;
            }
            m_position += size;
            bufPos += size;
            size -= size;
        }
        //如果当前的块不能放下剩下的所有数据
        else
        {
            memcpy(m_cur->ptr + nowPos, (const char *)buf + bufPos, nowCap);
            m_position += nowCap;
            bufPos += nowCap;
            size -= nowCap;
            m_cur = m_cur->next;
            nowCap = m_cur->size;
            nowPos = 0;
        }
    }
    //放好了，更新m_size
    m_size = m_position;
}

void ByteArray::read(void *buf, size_t size)
{
    if (size > getReadSize())
    {
        throw std::out_of_range("尝试读取，但是想读的太多，实际没那么多");
    }
    //后面的代码还没有看,感觉是有问题的，我先假设，m_position是当前读取的位置，也就是说在读之前
    // m_position是从末尾移到前面去了
    size_t nowPos = m_position % m_baseSize;
    // 当前块从当前位置开始，还剩多少字节可读
    size_t nowCap = m_cur->size - nowPos;
    // bufPos表示buf已经读取了多少
    size_t bufPos = 0;
    while (size > 0)
    {
        if (nowCap >= size)
        {

            memcpy((char *)buf + bufPos, m_cur->ptr + nowPos, size);
            if (m_cur->size == (nowPos + size))
            {
                m_cur = m_cur->next;
            }
            m_position += size;
            bufPos += size;
            size = 0;
        }
        else
        {
            memcpy((char *)buf + bufPos, m_cur->ptr + nowPos, nowCap);
            m_position += nowCap;
            bufPos += nowCap;
            size -= nowCap;
            m_cur = m_cur->next;
            nowCap = m_cur->size;
            nowPos = 0;
        }
    }
}

void ByteArray::read(void *buf, size_t size, size_t position) const
{
    if (size > (m_size - position))
    {
        throw std::out_of_range("尝试读取，但是想读的太多，实际没那么多");
    }
    //后面的代码还没有看,感觉是有问题的，我先假设，m_position是当前读取的位置，也就是说在读之前
    // m_position是从末尾移到前面去了
    size_t nowPos = position % m_baseSize;
    // 当前块从当前位置开始，还剩多少字节可读
    size_t nowCap = m_cur->size - nowPos;
    // bufPos表示buf已经读取了多少
    size_t bufPos = 0;
    Node *cur = m_cur;
    while (size > 0)
    {
        if (nowCap >= size)
        {

            memcpy((char *)buf + bufPos, cur->ptr + nowPos, size);
            if (cur->size == (nowPos + size))
            {
                cur = cur->next;
            }
            position += size;
            bufPos += size;
            size = 0;
        }
        else
        {
            memcpy((char *)buf + bufPos, cur->ptr + nowPos, nowCap);
            position += nowCap;
            bufPos += nowCap;
            size -= nowCap;
            cur = cur->next;
            nowCap = cur->size;
            nowPos = 0;
        }
    }
}

// 找到position对应的块
void ByteArray::setPosition(size_t position)
{
    if (position > m_capacity)
    {
        throw std::out_of_range("想要设置的position大于现在已写入的最大位置");
    }
    m_position = position;
    if (m_position > m_size)
    {
        m_size = m_position;
    }
    m_cur = m_root;
    while (position > m_cur->size)
    {
        position -= m_cur->size;
        m_cur = m_cur->next;
    }
    if (position == m_cur->size)
    {
        m_cur = m_cur->next;
    }
}

bool ByteArray::writeToFile(const std::string &name)
{
    std::ofstream ofs;
    ofs.open(name, std::ios::binary | std::ios::trunc);
    if (!ofs)
    {
        throw std::runtime_error("open file failed");
    }
    //拿到可读的大小
    size_t read_size = getReadSize();
    //不修改游标
    size_t postion = m_position;
    Node *cur = m_cur;
    //一直往后读
    while (read_size > 0)
    {
        //当前的块还能当前位置开始，还剩多少字节可读
        int diff = postion % m_baseSize;
        //这一次要读取多少
        size_t len = (read_size > m_baseSize ? m_baseSize : read_size) - diff;
        ofs.write(cur->ptr + diff, len);
        cur = cur->next;
        postion += len;
        read_size -= len;
    }
    return true;
}

bool ByteArray::readFromFile(const std::string &name)
{
    std::ifstream ifs;
    ifs.open(name, std::ios::binary);
    if (!ifs)
    {
        SYLAR_LOG_ERROR(g_logger) << "readFromFile name=" << name << " error, errno=" << errno
                                  << " errstr=" << strerror(errno);
        return false;
    }
    std::shared_ptr<char> buff(new char[m_baseSize], [](char *p) { delete[] p; });
    while (!ifs.eof())
    {
        ifs.read(buff.get(), m_baseSize);
        write(buff.get(), ifs.gcount());
    }
    return true;
}

std::string ByteArray::toString() const
{
    std::string str;
    str.resize(getReadSize());
    if (str.empty())
    {
        return str;
    }
    read(&str[0], str.size(), m_position);
    return str;
}
std::string ByteArray::toHexString() const
{
    std::string str = toString();
    std::stringstream ss;

    for (size_t i = 0; i < str.size(); ++i)
    {
        if (i > 0 && i % 32 == 0)
        {
            ss << std::endl;
        }
        ss << std::setw(2) << std::setfill('0') << std::hex << (int)(uint8_t)str[i] << " ";
    }

    return ss.str();
}

//用iovec的方式去取出数据做进一步的处理
uint64_t ByteArray::getReadBuffers(std::vector<iovec> &buffers, uint64_t len) const
{
    len = len > getReadSize() ? getReadSize() : len;
    if (len == 0)
    {
        return 0;
    }
    uint64_t size = len;

    size_t nowPos = m_position % m_baseSize;
    // 当前块从当前位置开始，还剩多少字节可读
    size_t nowCap = m_cur->size - nowPos;
    struct iovec iov;

    Node *cur = m_cur;
    while (len > 0)
    {
        if (nowCap > len)
        {
            iov.iov_base = cur->ptr + nowPos;
            iov.iov_len = len;
            len = 0;
        }
        else
        {
            iov.iov_base = cur->ptr + nowPos;
            iov.iov_len = nowCap;
            len -= nowCap;
            nowPos = 0;
            nowCap = cur->size;
            cur = cur->next;
        }
        buffers.push_back(iov);
    }
    return size;
}

uint64_t
ByteArray::getReadBuffers(std::vector<iovec> &buffers, uint64_t len, uint64_t position) const
{
    len = len > getReadSize() ? getReadSize() : len;
    if (len == 0)
    {
        return 0;
    }

    uint64_t size = len;

    size_t npos = position % m_baseSize;
    size_t count = position / m_baseSize;
    Node *cur = m_root;
    while (count > 0)
    {
        cur = cur->next;
        --count;
    }

    size_t ncap = cur->size - npos;
    struct iovec iov;
    while (len > 0)
    {
        if (ncap >= len)
        {
            iov.iov_base = cur->ptr + npos;
            iov.iov_len = len;
            len = 0;
        }
        else
        {
            iov.iov_base = cur->ptr + npos;
            iov.iov_len = ncap;
            len -= ncap;
            cur = cur->next;
            ncap = cur->size;
            npos = 0;
        }
        buffers.push_back(iov);
    }
    return size;
}

uint64_t ByteArray::getWriteBuffers(std::vector<iovec> &buffers, uint64_t len)
{
    if (len == 0)
    {
        return 0;
    }
    addCapacity(len);
    uint64_t size = len;

    size_t npos = m_position % m_baseSize;
    size_t ncap = m_cur->size - npos;
    struct iovec iov;
    Node *cur = m_cur;
    while (len > 0)
    {
        if (ncap >= len)
        {
            iov.iov_base = cur->ptr + npos;
            iov.iov_len = len;
            len = 0;
        }
        else
        {
            iov.iov_base = cur->ptr + npos;
            iov.iov_len = ncap;

            len -= ncap;
            cur = cur->next;
            ncap = cur->size;
            npos = 0;
        }
        buffers.push_back(iov);
    }
    return size;
}

//扩容操作
// 参数 size_t size,是要扩展成的大小，而不是几倍几倍这种
void ByteArray::addCapacity(size_t size)
{
    // 扩容操作，如果当前的容量已经足够了，那么就不扩容
    if (size == 0)
    {
        return;
    }
    size_t old_cap = getCapacity();
    if (old_cap >= size)
    {
        return;
    }
    // 如果当前的容量不足
    size -= old_cap;

    // 判断要加几个节点
    int count = size / m_baseSize + (size % m_baseSize > 0 ? 1 : 0);
    Node *tmp = m_root;
    while (tmp->next)
    {
        tmp = tmp->next;
    }
    // 要判断加完节点以后上一个节点是不是已经用完了，用完了就直接过渡到下一个节点去
    Node *first = nullptr;
    for (int i = 0; i < count; i++)
    {
        tmp->next = new Node(m_baseSize);
        if (first == nullptr)
        {
            first = tmp->next;
        }
        m_capacity += m_baseSize;
        tmp = tmp->next;
    }
    if (old_cap == 0)
    {
        m_cur = first;
    }
}

} // namespace sylar
