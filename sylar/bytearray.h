#ifndef SYLAR_BYTEARRAY_H
#define SYLAR_BYTEARRAY_H
#include <bits/types/struct_iovec.h>
#include <cstddef>
#include <memory>
#include <stdint.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <vector>
namespace sylar
{
class ByteArray
{
public:
    typedef std::shared_ptr<ByteArray> ptr;
    struct Node
    {
        Node(size_t s);
        Node();
        ~Node();
        //这个块儿的起始地址
        char *ptr;
        Node *next;
        //当前这个块儿的最大容量
        size_t size;
    };

    ByteArray(size_t base_size = 4096);
    ~ByteArray();

    // write (固定大小)
    void writeFint8(int8_t value);
    void writeFuint8(uint8_t value);
    void writeFint16(int16_t value);
    void writeFuint16(uint16_t value);
    void writeFint32(int32_t value);
    void writeFuint32(uint32_t value);
    void writeFint64(int64_t value);
    void writeFuint64(uint64_t value);

    // write (可变大小)
    void writeInt32(int32_t value);
    void writeUint32(uint32_t value);
    void writeInt64(int64_t value);
    void writeUint64(uint64_t value);

    void writeFloat(float value);
    void writeDouble(double value);

    // string
    //  length:int16 , data
    void writeStringF16(const std::string &value);
    // length:int32 , data
    void writeStringF32(const std::string &value);
    // length:int64 , data
    void writeStringF64(const std::string &value);
    // length:varint, data
    void writeStringVint(const std::string &value);
    // data
    void writeStringWithoutLength(const std::string &value);

    // read
    int8_t readFint8();
    uint8_t readFuint8();
    int16_t readFint16();
    uint16_t readFuint16();
    int32_t readFint32();
    uint32_t readFuint32();
    int64_t readFint64();
    uint64_t readFuint64();

    int32_t readInt32();
    uint32_t readUint32();
    int64_t readInt64();
    uint64_t readUint64();

    float readFloat();
    double readDouble();

    // length:int16, data
    std::string readStringF16();
    // length:int32, data
    std::string readStringF32();
    // length:int64, data
    std::string readStringF64();
    // length:varint , data
    std::string readStringVint();

    void clear();

    void write(const void *buf, size_t size);
    void read(void *buf, size_t size);
    void read(void *buf, size_t size, size_t position) const;

    size_t getPosition()
    {
        return m_position;
    }
    void setPosition(size_t position);

    bool writeToFile(const std::string &name);
    bool readFromFile(const std::string &name);

    size_t getBaseSize() const
    {
        return m_baseSize;
    }

    // 当前还能读多少(已经写入了的，还剩多少没有读)
    size_t getReadSize() const
    {
        return m_size - m_position;
    }

    bool isLittleIndian();
    void setLittleIndian(bool val);

    std::string toString() const;
    std::string toHexString() const;

    //只读取这一片儿的内容但是不改变内部的position，使得下次还是从那里开始读
    uint64_t getReadBuffers(std::vector<iovec> &buffers, uint64_t len = ~0ull) const;
    uint64_t getReadBuffers(std::vector<iovec> &buffers, uint64_t len, uint64_t position) const;
    //增加容量，不修改position
    uint64_t getWriteBuffers(std::vector<iovec> &buffers, uint64_t len);

    size_t getSize() const
    {
        return m_size;
    }

private:
    void addCapacity(size_t size);
    //从当前位置往后还能直接写多少字节
    size_t getCapacity() const
    {
        return m_capacity - m_position;
    }

private:
    //初始大小
    size_t m_baseSize;
    //读取/写入的位置(总的偏移量)
    size_t m_position;
    // 当前总容量
    size_t m_capacity;
    // 已经写入过的有效数据总长度
    size_t m_size;

    //希望网络传输是以大端还是小端进行传输
    int8_t m_endian;
    // 链表头结点
    Node *m_root;
    // 当前读写到的节点（这个position当前落在哪个块里）
    Node *m_cur;
};
} // namespace sylar
#endif