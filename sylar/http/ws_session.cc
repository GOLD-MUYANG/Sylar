#include "ws_session.h"

#include "sylar/endian.h" // byteswapOnLittleEndian，用于网络字节序和主机字节序转换
#include "sylar/log.h"    // 日志系统
#include "sylar/util/hash_util.h" // sha1sum、base64encode
#include <string.h>               // memset、memcpy

namespace sylar
{
namespace http
{

/**
 * WebSocket 模块使用的日志器。
 */
static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

/**
 * WebSocket 单条消息最大大小配置。
 *
 * 默认值：
 *   1024 * 1024 * 32 = 32MB
 *
 * 用途：
 *   接收 WebSocket 分片时不断累加 payload 长度。
 *   如果超过该限制，就中断并关闭连接，防止异常客户端撑爆内存。
 */
sylar::ConfigVar<uint32_t>::ptr g_websocket_message_max_size = sylar::Config::Lookup(
    "websocket.message.max_size", (uint32_t)1024 * 1024 * 32, "websocket message max size");

/**
 * WSSession 构造函数。
 *
 * 这里只是把 socket 和 owner 交给 HttpSession。
 *
 * WSSession 继承 HttpSession 的原因：
 *   WebSocket 握手阶段仍然是 HTTP 协议。
 *   握手完成后才开始收发 WebSocket frame。
 */
WSSession::WSSession(Socket::ptr sock, bool owner) : HttpSession(sock, owner)
{
}

/**
 * 服务端处理 WebSocket 握手。
    符合条件就返回websocket协议的rsp了
 *
 * 客户端请求大概长这样：
 *
 *   GET /path HTTP/1.1
 *   Host: xxx
 *   Upgrade: websocket
 *   Connection: Upgrade
 *   Sec-WebSocket-Key: xxxxx
 *   Sec-WebSocket-Version: 13
 *
 * 服务端返回：
 *
 *   HTTP/1.1 101 Switching Protocols
 *   Upgrade: websocket
 *   Connection: Upgrade
 *   Sec-WebSocket-Accept: base64(sha1(key + GUID))
 *
 * 关键点：
 *   Sec-WebSocket-Accept 的算法是协议规定的。
 *   GUID 固定为：
 *     258EAFA5-E914-47DA-95CA-C5AB0DC85B11
 *
 * @return 成功返回 HTTP 请求对象，失败返回 nullptr
 */
HttpRequest::ptr WSSession::handleShake()
{
    HttpRequest::ptr req;

    do
    {
        /**
         * 读取客户端发来的 HTTP 请求。
         *
         * WebSocket 握手第一步仍然是 HTTP 请求，
         * 所以这里调用 HttpSession::recvRequest()。
         */
        req = recvRequest();

        if (!req)
        {
            SYLAR_LOG_INFO(g_logger) << "invalid http request";
            break;
        }

        /**
         * 这里原本可以检查：
         *
         *   Upgrade: websocket
         *   Connection: Upgrade
         *
         * 但当前源码把检查注释掉了。
         *
         * 风险：
         *   非标准 WebSocket Upgrade 请求也可能进入后续逻辑。
         *
         * 更严谨的写法应该检查这两个头字段。
         */
        // if(req->getHeader("Upgrade") != "websocket") {
        //     SYLAR_LOG_INFO(g_logger) << "http header Upgrade != websocket";
        //     break;
        // }
        // if(req->getHeader("Connection") != "Upgrade") {
        //     SYLAR_LOG_INFO(g_logger) << "http header Connection != Upgrade";
        //     break;
        // }

        /**
         * 检查 WebSocket 版本。
         *
         * RFC 6455 标准使用版本 13。
         *
         * 注意：
         *   你这里写的是 "Sec-webSocket-Version"。
         *   HTTP header 通常应该大小写不敏感。
         *   如果 HttpRequest 内部 map 做了大小写归一化，就没问题。
         *   如果内部是大小写敏感，这里可能读不到客户端的
         *   "Sec-WebSocket-Version"。
         */
        if (req->getHeaderAs<int>("Sec-webSocket-Version") != 13)
        {
            SYLAR_LOG_INFO(g_logger) << "http header Sec-webSocket-Version != 13";
            break;
        }

        /**
         * 获取客户端发来的 Sec-WebSocket-Key。
         *
         * 这是客户端随机生成的 base64 字符串。
         * 服务端不能原样返回，需要拼接固定 GUID 后做 sha1 + base64。
         */
        std::string key = req->getHeader("Sec-WebSocket-Key");

        if (key.empty())
        {
            SYLAR_LOG_INFO(g_logger) << "http header Sec-WebSocket-Key = null";
            break;
        }

        /**
         * 计算 Sec-WebSocket-Accept。
         *
         * 公式：
         *
         *   accept = base64(sha1(key + GUID))
         *
         * GUID 是 WebSocket 协议固定值。
         */
        std::string v = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        v = sylar::base64encode(sylar::sha1sum(v));

        /**
         * 基于请求对象创建响应对象。
         *
         * createResponse() 一般会继承请求的版本号、keep-alive 等信息。
         */
        auto rsp = req->createResponse();

        /**
         * HTTP 状态码 101：
         *   Switching Protocols
         *
         * 表示协议从 HTTP 切换到 WebSocket。
         */
        rsp->setStatus(HttpStatus::SWITCHING_PROTOCOLS);

        /**
         * 标记这个响应是 WebSocket 响应。
         *
         * 具体作用取决于 HttpResponse::dump() 等实现。
         */
        rsp->setWebsocket(true);

        /**
         * 设置状态原因短语。
         */
        rsp->setReason("Web Socket Protocol Handshake");

        /**
         * 设置 WebSocket Upgrade 必要响应头。
         */
        rsp->setHeader("Upgrade", "websocket");
        rsp->setHeader("Connection", "Upgrade");
        rsp->setHeader("Sec-WebSocket-Accept", v);

        /**
         * 发送 101 响应。
         *
         * 发送成功后，这条 TCP 连接后续就不再走 HTTP 报文格式，
         * 而是走 WebSocket frame 格式。
         */
        sendResponse(rsp);

        /**
         * 打印请求和响应，方便调试握手过程。
         */
        SYLAR_LOG_INFO(g_logger) << *req;
        SYLAR_LOG_INFO(g_logger) << *rsp;

        return req;

    } while (false);

    /**
     * 握手失败时，如果 req 不为空，打印请求内容，方便定位是哪个头字段不合格。
     */
    if (req)
    {
        SYLAR_LOG_INFO(g_logger) << *req;
    }

    return nullptr;
}

/**
 * WSFrameMessage 构造函数。
 *
 * @param opcode WebSocket 消息类型
 * @param data   消息数据
 */
WSFrameMessage::WSFrameMessage(int opcode, const std::string &data) : m_opcode(opcode), m_data(data)
{
}

/**
 * 把 WebSocket 帧头转成字符串。
 *
 * 用于日志调试。
 */
std::string WSFrameHead::toString() const
{
    std::stringstream ss;

    ss << "[WSFrameHead fin=" << fin << " rsv1=" << rsv1 << " rsv2=" << rsv2 << " rsv3=" << rsv3
       << " opcode=" << opcode << " mask=" << mask << " payload=" << payload << "]";

    return ss.str();
}

/**
 * WSSession 成员函数：接收消息。
 *
 * false 表示当前是服务端接收逻辑。
 * 服务端收到客户端帧时，按协议要求客户端帧必须 mask。
 */
WSFrameMessage::ptr WSSession::recvMessage()
{
    return WSRecvMessage(this, false);
}

/**
 * WSSession 成员函数：发送消息对象。
 *
 * false 表示当前是服务端发送逻辑。
 * 服务端发给客户端时，不需要 mask。
 */
int32_t WSSession::sendMessage(WSFrameMessage::ptr msg, bool fin)
{
    return WSSendMessage(this, msg, false, fin);
}

/**
 * WSSession 成员函数：发送字符串消息。
 *
 * 内部临时构造 WSFrameMessage。
 */
int32_t WSSession::sendMessage(const std::string &msg, int32_t opcode, bool fin)
{
    return WSSendMessage(this, std::make_shared<WSFrameMessage>(opcode, msg), false, fin);
}

/**
 * WSSession 成员函数：发送 ping。
 */
int32_t WSSession::ping()
{
    return WSPing(this);
}

/**
 * 从 Stream 中接收一条完整 WebSocket 消息。
 *
 * 参数 client 的含义：
 *
 *   client = false：
 *     当前作为服务端接收客户端数据。
 *     按协议，客户端发来的帧必须 mask = 1。
 *
 *   client = true：
 *     当前作为客户端接收服务端数据。
 *     服务端发来的帧通常 mask = 0。
 *
 * 返回：
 *   成功：WSFrameMessage::ptr
 *   失败：nullptr，并关闭 stream
 */
WSFrameMessage::ptr WSRecvMessage(Stream *stream, bool client)
{
    /**
     * opcode 用来记录完整消息的类型。
     *
     * 如果消息没有分片：
     *   首帧 opcode 就是 TEXT_FRAME 或 BIN_FRAME。
     *
     * 如果消息有分片：
     *   第一帧 opcode 是 TEXT_FRAME 或 BIN_FRAME。
     *   后续帧 opcode 是 CONTINUE。
     *
     * 所以这里需要保存第一帧的 opcode。
     */
    int opcode = 0;

    /**
     * data 用来累积完整消息的数据。
     *
     * 多个分片会不断追加到这个 string 里。
     */
    std::string data;

    /**
     * 当前已经读取的 payload 长度。
     *
     * 用于追加分片，也用于判断是否超过最大消息大小。
     */
    int cur_len = 0;

    do
    {
        /**
         * 读取 2 字节基础 WebSocket 帧头。
         *
         * WSFrameHead 被 #pragma pack(1) 压成紧凑布局，
         * sizeof(ws_head) 期望是 2。
         *
         * 风险：
         *   位域布局依赖编译器和平台。
         *   更可移植做法是读取两个 uint8_t，然后用位运算解析。
         */
        WSFrameHead ws_head;

        if (stream->readFixSize(&ws_head, sizeof(ws_head)) <= 0)
        {
            break;
        }

        SYLAR_LOG_DEBUG(g_logger) << "WSFrameHead " << ws_head.toString();

        /**
         * 处理 PING 帧。
         *
         * WebSocket 收到 ping 后一般要回复 pong。
         */
        if (ws_head.opcode == WSFrameHead::PING)
        {
            SYLAR_LOG_INFO(g_logger) << "PING";

            if (WSPong(stream) <= 0)
            {
                break;
            }
        }
        /**
         * 处理 PONG 帧。
         *
         * 当前实现没有额外逻辑，直接忽略。
         */
        else if (ws_head.opcode == WSFrameHead::PONG)
        {
        }
        /**
         * 处理数据帧：
         *
         *   CONTINUE   : 后续分片
         *   TEXT_FRAME : 文本消息首帧或完整帧
         *   BIN_FRAME  : 二进制消息首帧或完整帧
         */
        else if (ws_head.opcode == WSFrameHead::CONTINUE ||
                 ws_head.opcode == WSFrameHead::TEXT_FRAME ||
                 ws_head.opcode == WSFrameHead::BIN_FRAME)
        {
            /**
             * 服务端接收客户端数据时，客户端必须 mask。
             *
             * client = false 代表当前是服务端角色。
             *
             * 如果客户端发来的数据没有 mask，属于协议错误。
             */
            if (!client && !ws_head.mask)
            {
                SYLAR_LOG_INFO(g_logger) << "WSFrameHead mask != 1";
                break;
            }

            /**
             * 真实 payload 长度。
             *
             * ws_head.payload 只有 7 bit。
             * 当它等于 126 或 127 时，需要继续读取扩展长度。
             */
            uint64_t length = 0;

            if (ws_head.payload == 126)
            {
                /**
                 * payload == 126：
                 *   后续 2 字节表示真实长度。
                 *
                 * 网络传输使用大端序。
                 * 本机如果是小端，需要转换。
                 */
                uint16_t len = 0;

                if (stream->readFixSize(&len, sizeof(len)) <= 0)
                {
                    break;
                }

                length = sylar::byteswapOnLittleEndian(len);
            }
            else if (ws_head.payload == 127)
            {
                /**
                 * payload == 127：
                 *   后续 8 字节表示真实长度。
                 */
                uint64_t len = 0;

                if (stream->readFixSize(&len, sizeof(len)) <= 0)
                {
                    break;
                }

                length = sylar::byteswapOnLittleEndian(len);
            }
            else
            {
                /**
                 * payload < 126：
                 *   payload 字段本身就是真实长度。
                 */
                length = ws_head.payload;
            }

            /**
             * 检查消息累计长度是否超过配置上限。
             *
             * 当前判断是：
             *   (cur_len + length) >= max_size
             *
             * 这意味着刚好等于 max_size 也会被拒绝。
             *
             * 如果你希望最大允许值包含 32MB，可以改成：
             *   > max_size
             */
            if ((cur_len + length) >= g_websocket_message_max_size->getValue())
            {
                SYLAR_LOG_WARN(g_logger)
                    << "WSFrameMessage length > " << g_websocket_message_max_size->getValue()
                    << " (" << (cur_len + length) << ")";
                break;
            }

            /**
             * 读取 masking-key。
             *
             * 如果 mask = 1，则 payload 前面有 4 字节 masking-key。
             *
             * 客户端发给服务端的数据必须带 mask。
             * 服务端收到后需要用这个 key 对 payload 逐字节异或解码。
             */
            char mask[4] = {0};

            if (ws_head.mask)
            {
                if (stream->readFixSize(mask, sizeof(mask)) <= 0)
                {
                    break;
                }
            }

            /**
             * 扩大 data 空间，用于容纳当前分片。
             *
             * cur_len 是之前已经读取的长度。
             * length 是当前帧 payload 长度。
             */
            data.resize(cur_len + length);

            /**
             * 读取当前帧的 payload 到 data[cur_len] 开始的位置。
             */
            if (stream->readFixSize(&data[cur_len], length) <= 0)
            {
                break;
            }

            /**
             * 如果当前帧带 mask，则进行解码。
             *
             * WebSocket mask 解码公式：
             *
             *   decoded[i] = encoded[i] ^ mask[i % 4]
             */
            if (ws_head.mask)
            {
                for (int i = 0; i < (int)length; ++i)
                {
                    data[cur_len + i] ^= mask[i % 4];
                }
            }

            /**
             * 更新已读取长度。
             */
            cur_len += length;

            /**
             * 记录完整消息的 opcode。
             *
             * 分片消息中：
             *   第一帧是 TEXT_FRAME 或 BIN_FRAME。
             *   后续帧是 CONTINUE。
             *
             * 所以只在 opcode 还没设置，并且当前帧不是 CONTINUE 时记录。
             */
            if (!opcode && ws_head.opcode != WSFrameHead::CONTINUE)
            {
                opcode = ws_head.opcode;
            }

            /**
             * 如果 fin = 1，说明完整消息已经收完。
             *
             * 返回 WSFrameMessage。
             */
            if (ws_head.fin)
            {
                SYLAR_LOG_DEBUG(g_logger) << data;

                return WSFrameMessage::ptr(new WSFrameMessage(opcode, std::move(data)));
            }
        }
        else
        {
            /**
             * 不支持的 opcode。
             *
             * 当前实现只是打印日志，然后继续循环。
             * 更严格的实现应该关闭连接或返回协议错误。
             */
            SYLAR_LOG_DEBUG(g_logger) << "invalid opcode=" << ws_head.opcode;
        }

    } while (true);

    /**
     * 任何读取失败、协议错误、长度超限，都会关闭 stream。
     */
    stream->close();

    return nullptr;
}

/**
 * 向 Stream 发送一条 WebSocket 消息。
 *
 * 参数 client 的含义：
 *
 *   client = true：
 *     当前作为客户端发送。
 *     客户端发给服务端必须 mask。
 *
 *   client = false：
 *     当前作为服务端发送。
 *     服务端发给客户端通常不 mask。
 *
 * 返回值：
 *   成功：当前返回 size + sizeof(ws_head)
 *   失败：-1，并关闭 stream
 *
 * 注意：
 *   当前返回值没有把扩展长度字段和 mask 字段算进去。
 *   所以它不是严格的“实际写入字节数”。
 */
int32_t WSSendMessage(Stream *stream, WSFrameMessage::ptr msg, bool client, bool fin)
{
    do
    {
        /**
         * 构造基础帧头。
         */
        WSFrameHead ws_head;

        /**
         * 清零，保证 rsv/opcode/payload/mask 等初始状态确定。
         */
        memset(&ws_head, 0, sizeof(ws_head));

        /**
         * 设置 FIN。
         *
         * fin = true  ：当前帧是完整消息或最后一个分片。
         * fin = false ：后面还有分片。
         */
        ws_head.fin = fin;

        /**
         * 设置 opcode。
         *
         * 普通文本消息一般是 TEXT_FRAME。
         * 二进制消息一般是 BIN_FRAME。
         */
        ws_head.opcode = msg->getOpcode();

        /**
         * 设置 mask 标记。
         *
         * client = true 表示客户端发送，需要 mask。
         * client = false 表示服务端发送，不需要 mask。
         */
        ws_head.mask = client;

        /**
         * 获取 payload 大小。
         */
        uint64_t size = msg->getData().size();

        /**
         * 根据 payload 大小设置长度标记。
         *
         * size < 126：
         *   直接放入 7 bit payload 字段。
         *
         * 126 <= size < 65536：
         *   payload 字段写 126，
         *   后面额外写 2 字节长度。
         *
         * size >= 65536：
         *   payload 字段写 127，
         *   后面额外写 8 字节长度。
         */
        if (size < 126)
        {
            ws_head.payload = size;
        }
        else if (size < 65536)
        {
            ws_head.payload = 126;
        }
        else
        {
            ws_head.payload = 127;
        }

        /**
         * 先写基础 2 字节帧头。
         */
        if (stream->writeFixSize(&ws_head, sizeof(ws_head)) <= 0)
        {
            break;
        }

        /**
         * 如果 payload == 126，额外写 2 字节长度。
         *
         * WebSocket 使用网络字节序，也就是大端序。
         * 所以在小端机器上需要 byteswap。
         */
        if (ws_head.payload == 126)
        {
            uint16_t len = size;
            len = sylar::byteswapOnLittleEndian(len);

            if (stream->writeFixSize(&len, sizeof(len)) <= 0)
            {
                break;
            }
        }
        /**
         * 如果 payload == 127，额外写 8 字节长度。
         */
        else if (ws_head.payload == 127)
        {
            uint64_t len = sylar::byteswapOnLittleEndian(size);

            if (stream->writeFixSize(&len, sizeof(len)) <= 0)
            {
                break;
            }
        }

        /**
         * 客户端发送时，需要 mask payload。
         *
         * 当前实现：
         *   1. 用 rand() 生成 4 字节 mask
         *   2. 直接修改 msg->getData() 里的原始数据
         *   3. 先写 mask
         *   4. 再写被 mask 过的数据
         *
         * 风险 1：
         *   rand() 随机性较弱，但这里通常只是协议 mask，不是加密。
         *
         * 风险 2：
         *   直接修改 msg->getData() 有副作用。
         *   如果调用者发送后还想继续使用原始 msg 数据，会发现数据已经被异或改掉。
         *
         * 更稳的写法：
         *   拷贝一份 data 副本，对副本 mask，然后发送副本。
         */
        if (client)
        {
            char mask[4];

            uint32_t rand_value = rand();
            memcpy(mask, &rand_value, sizeof(mask));

            std::string &data = msg->getData();

            for (size_t i = 0; i < data.size(); ++i)
            {
                data[i] ^= mask[i % 4];
            }

            /**
             * 写 4 字节 masking-key。
             */
            if (stream->writeFixSize(mask, sizeof(mask)) <= 0)
            {
                break;
            }
        }

        /**
         * 写 payload 数据。
         *
         * 如果 client = true，这里的数据已经被 mask。
         * 如果 client = false，这里的数据是原始数据。
         */
        if (stream->writeFixSize(msg->getData().c_str(), size) <= 0)
        {
            break;
        }

        /**
         * 当前返回值只包含：
         *   payload size + 基础帧头大小
         *
         * 没有包含：
         *   扩展长度字段 2/8 字节
         *   mask 字段 4 字节
         *
         * 所以它只能表示“大致发送成功”，不能当成精确发送字节数。
         */
        return size + sizeof(ws_head);

    } while (0);

    /**
     * 发送失败，关闭 stream。
     */
    stream->close();

    return -1;
}

/**
 * WSSession 成员函数：发送 pong。
 */
int32_t WSSession::pong()
{
    return WSPong(this);
}

/**
 * 发送 ping 帧。
 *
 * 当前实现发送的是空 payload ping。
 *
 * WebSocket 控制帧要求：
 *   - payload 长度 <= 125
 *   - 不能分片
 *
 * 这里 payload 默认 0，符合要求。
 */
int32_t WSPing(Stream *stream)
{
    WSFrameHead ws_head;

    memset(&ws_head, 0, sizeof(ws_head));

    /**
     * ping 是完整控制帧，fin 必须为 1。
     */
    ws_head.fin = 1;

    /**
     * 设置 opcode 为 PING。
     */
    ws_head.opcode = WSFrameHead::PING;

    /**
     * 写出基础帧头。
     *
     * payload 默认是 0。
     * mask 默认是 0。
     *
     * 注意：
     *   如果这里作为客户端发送 ping，按协议应该 mask = 1。
     *   但当前函数没有 client 参数，所以它更适合服务端使用。
     */
    int32_t v = stream->writeFixSize(&ws_head, sizeof(ws_head));

    if (v <= 0)
    {
        stream->close();
    }

    return v;
}

/**
 * 发送 pong 帧。
 *
 * 当前实现发送的是空 payload pong。
 */
int32_t WSPong(Stream *stream)
{
    WSFrameHead ws_head;

    memset(&ws_head, 0, sizeof(ws_head));

    /**
     * pong 是完整控制帧，fin 必须为 1。
     */
    ws_head.fin = 1;

    /**
     * 设置 opcode 为 PONG。
     */
    ws_head.opcode = WSFrameHead::PONG;

    /**
     * 写出基础帧头。
     *
     * 注意：
     *   如果这里作为客户端发送 pong，按协议也应该 mask = 1。
     *   当前函数没有 client 参数，所以更适合服务端回复客户端 ping。
     */
    int32_t v = stream->writeFixSize(&ws_head, sizeof(ws_head));

    if (v <= 0)
    {
        stream->close();
    }

    return v;
}

} // namespace http
} // namespace sylar