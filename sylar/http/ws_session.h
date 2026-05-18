#ifndef __SYLAR_HTTP_WS_SESSION_H__
#define __SYLAR_HTTP_WS_SESSION_H__
#include "sylar/config.h" // 配置系统，用于声明 websocket 最大消息大小配置项
#include "sylar/http/http_session.h" // HTTP 会话基类，WebSocket 握手阶段依赖 HTTP 请求/响应
#include <stdint.h>                  // uint32_t 等固定宽度整数类型

/**
它先用 HTTP 握手建立连接
握手成功后，同一个 TCP 连接不再传 HTTP 报文
之后双方传的是 WebSocket Frame
一个业务消息可以由一个或多个 Frame 组成
Frame 里有 opcode、fin、payload length、mask、payload data
服务端和客户端的 mask 规则不一样

一个 Frame 的基础结构可以理解成：

+--------+--------+----------------+--------------+--------------+
| FIN等  | MASK等 | 扩展长度        | Masking Key  | Payload Data |
+--------+--------+----------------+--------------+--------------+
*/
namespace sylar
{
namespace http
{

/**
 * WebSocket 帧头结构。
 *
 * WebSocket 协议的基础帧头至少 2 字节：
 *
 * 第 1 字节：
 * FIN RSV1 RSV2 RSV3 opcode

 * 第 2 字节：
 * MASK payload_len

 * 第 1 字节：
 *   bit 7      : FIN，是否是消息的最后一个分片
 *   bit 6 ~ 4  : RSV1/RSV2/RSV3，扩展位，普通情况下必须为 0
 *   bit 3 ~ 0  : opcode，帧类型
 *
 * 第 2 字节：
 *   bit 7      : MASK，客户端发给服务端必须为 1，服务端发给客户端通常为 0
 *   bit 6 ~ 0  : payload len，负载长度标记
 *
 * 注意：
 *   这里用 C++ 位域直接映射网络字节流。
 *   这种写法依赖编译器位域布局，严格来说可移植性不好。
 *   在学习 Sylar 源码时可以这样理解，但工业代码更推荐手动读 uint8_t 后位运算解析。
 */
#pragma pack(1)
struct WSFrameHead
{
    /**
     * WebSocket opcode 类型。
     *
     * 常见值：
     *   0x0 : 继续帧，用于分片消息的后续帧
     *   0x1 : 文本帧
     *   0x2 : 二进制帧
     *   0x8 : 关闭连接
     *   0x9 : ping
     *   0xA : pong
     */
    enum OPCODE
    {
        /// 数据分片帧，也叫 continuation frame
        CONTINUE = 0,

        /// 文本帧，payload 是 UTF-8 文本
        TEXT_FRAME = 1,

        /// 二进制帧，payload 是二进制数据
        BIN_FRAME = 2,

        /// 关闭连接帧
        CLOSE = 8,

        /// 心跳 ping 帧，对端收到后一般要回复 pong
        PING = 0x9,

        /// 心跳 pong 帧
        PONG = 0xA
    };

    /**
     * opcode，占 4 bit。
     *
     * 按 WebSocket 协议，opcode 决定当前帧的类型。
     * 例如 TEXT_FRAME、BIN_FRAME、PING、PONG。
     */
    uint32_t opcode : 4;

    /**
     * RSV3，占 1 bit。
     *
     * 默认应为 0。
     * 只有协商了扩展时才允许使用。
     */
    bool rsv3 : 1;

    /**
     * RSV2，占 1 bit。
     *
     * 默认应为 0。
     */
    bool rsv2 : 1;

    /**
     * RSV1，占 1 bit。
     *
     * 默认应为 0。
     * 某些压缩扩展可能使用 RSV1。
     */
    bool rsv1 : 1;

    /**
     * FIN，占 1 bit。
     *
     * fin = 1：当前帧是这个消息的最后一个分片。
     * fin = 0：后面还有 continuation frame。
     */
    bool fin : 1;

    /**
     * payload，占 7 bit。
     *
     * 该字段不是总能直接表示真实长度：
     *
     *   payload < 126：
     *       payload 本身就是真实长度。
     *
     *   payload == 126：
     *       后面额外读取 2 字节 uint16_t 作为真实长度。
     *
     *   payload == 127：
     *       后面额外读取 8 字节 uint64_t 作为真实长度。
     */
    uint32_t payload : 7;

    /**
     * mask，占 1 bit。
     *
     * 客户端发给服务端的帧必须 mask = 1。
     * 服务端发给客户端的帧一般 mask = 0。
     *
     * 如果 mask = 1，payload length 后面还有 4 字节 masking-key。
     */
    bool mask : 1;

    /**
     * 调试用：把当前帧头转成字符串。
     *
     * 主要用于日志输出，方便观察 fin/opcode/mask/payload 等字段。
     */
    std::string toString() const;
};
#pragma pack()

/**
 * WebSocket 消息对象。
 *
 * WSFrameHead 表示“单个帧”的头部。
 * WSFrameMessage 表示“业务层收到的一条完整消息”。
 *
 * 如果 WebSocket 消息被拆成多个帧：
 *   TEXT/BIN + CONTINUE + CONTINUE ...
 *
 * recvMessage() 会把多个分片的 payload 拼起来，
 * 最终返回一个 WSFrameMessage。
 */
class WSFrameMessage
{
public:
    typedef std::shared_ptr<WSFrameMessage> ptr;

    /**
     * 构造 WebSocket 消息。
     *
     * @param opcode 消息类型，例如 TEXT_FRAME / BIN_FRAME
     * @param data   消息内容
     */
    WSFrameMessage(int opcode = 0, const std::string &data = "");

    /**
     * 获取消息 opcode。
     *
     * 对完整消息来说，opcode 通常取首帧的 opcode。
     * 后续 CONTINUE 帧只负责追加数据。
     */
    int getOpcode() const
    {
        return m_opcode;
    }

    /**
     * 设置消息 opcode。
     */
    void setOpcode(int v)
    {
        m_opcode = v;
    }

    /**
     * 获取只读消息内容。
     *
     * 返回 const 引用，避免拷贝。
     */
    const std::string &getData() const
    {
        return m_data;
    }

    /**
     * 获取可修改的消息内容。
     *
     * 发送客户端 mask 数据时，当前实现会直接修改 m_data。
     * 这点有副作用，后面 .cc 注释里会说明。
     */
    std::string &getData()
    {
        return m_data;
    }

    /**
     * 设置消息内容。
     */
    void setData(const std::string &v)
    {
        m_data = v;
    }

private:
    /**
     * WebSocket 消息类型。
     *
     * 常见值：
     *   TEXT_FRAME
     *   BIN_FRAME
     *   CLOSE
     *   PING
     *   PONG
     */
    int m_opcode;

    /**
     * WebSocket 消息数据。
     */
    std::string m_data;
};

/**
 * WebSocket 会话类。
 *
 * WSSession 继承 HttpSession。
 *
 * 原因：
 *   WebSocket 连接一开始是 HTTP 请求。
 *   客户端先发 HTTP Upgrade 请求。
 *   服务端返回 101 Switching Protocols。
 *   握手成功之后，连接才进入 WebSocket 帧收发阶段。
 */
class WSSession : public HttpSession
{
public:
    typedef std::shared_ptr<WSSession> ptr;

    /**
     * 构造 WebSocket 会话。
     *
     * @param sock  底层 socket
     * @param owner 是否由当前 session 管理 socket 生命周期
     */
    WSSession(Socket::ptr sock, bool owner = true);

    /**
     * 服务端处理 WebSocket 握手。
     *
     * 流程：
     *   1. 读取 HTTP 请求
     *   2. 检查 Sec-WebSocket-Version
     *   3. 读取 Sec-WebSocket-Key
     *   4. 计算 Sec-WebSocket-Accept
     *   5. 返回 101 Switching Protocols
     *
     * @return 成功返回握手请求对象，失败返回 nullptr
     */
    HttpRequest::ptr handleShake();

    /**
     * 接收一条完整 WebSocket 消息。
     *
     * 内部会调用全局函数 WSRecvMessage(this, false)。
     *
     * false 表示当前是服务端逻辑：
     *   - 服务端接收客户端数据时，要求客户端帧必须带 mask。
     */
    WSFrameMessage::ptr recvMessage();

    /**
     * 发送一条 WebSocket 消息。
     *
     * @param msg 要发送的消息对象
     * @param fin 是否为最终分片，默认 true
     *
     * 当前 WSSession 用于服务端，所以底层调用 WSSendMessage(this, msg, false, fin)。
     * false 表示服务端发送给客户端时不使用 mask。
     */
    int32_t sendMessage(WSFrameMessage::ptr msg, bool fin = true);

    /**
     * 发送字符串消息。
     *
     * @param msg    字符串内容
     * @param opcode 默认 TEXT_FRAME，也可以传 BIN_FRAME
     * @param fin    是否最终分片
     */
    int32_t
    sendMessage(const std::string &msg, int32_t opcode = WSFrameHead::TEXT_FRAME, bool fin = true);

    /**
     * 发送 ping 帧。
     */
    int32_t ping();

    /**
     * 发送 pong 帧。
     */
    int32_t pong();

private:
    /**
     * 服务端握手。
     *
     * 当前头文件里声明了，但你贴出的 .cc 里没有实现。
     * 实际 handleShake() 已经直接完成服务端握手逻辑。
     */
    bool handleServerShake();

    /**
     * 客户端握手。
     *
     * 当前头文件里声明了，但你贴出的 .cc 里没有实现。
     */
    bool handleClientShake();
};

/**
 * WebSocket 单条消息最大大小配置。
 *
 * 在 .cc 中默认值是：
 *   32MB
 *
 * 用于防止对端发超大消息导致内存暴涨。
 */
extern sylar::ConfigVar<uint32_t>::ptr g_websocket_message_max_size;

/**
 * 从 Stream 中接收一条完整 WebSocket 消息。
 *
 * @param stream 底层流对象，可以是 SocketStream / HttpSession / WSSession
 * @param client 当前是否按客户端身份收帧
 *
 * client 的含义：
 *   client = false：服务端收客户端帧，要求对方 mask = 1
 *   client = true ：客户端收服务端帧，一般不要求 mask
 */
WSFrameMessage::ptr WSRecvMessage(Stream *stream, bool client);

/**
 * 向 Stream 写入一条 WebSocket 消息。
 *
 * @param stream 底层流对象
 * @param msg    要发送的消息
 * @param client 当前是否按客户端身份发送
 * @param fin    是否最终分片
 *
 * client 的含义：
 *   client = true ：客户端发给服务端，必须 mask = 1
 *   client = false：服务端发给客户端，通常 mask = 0
 */
int32_t WSSendMessage(Stream *stream, WSFrameMessage::ptr msg, bool client, bool fin);

/**
 * 发送 ping 帧。
 */
int32_t WSPing(Stream *stream);

/**
 * 发送 pong 帧。
 */
int32_t WSPong(Stream *stream);

} // namespace http
} // namespace sylar

#endif