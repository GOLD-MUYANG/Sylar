// 当前文件对应 WSServer 的实现
#include "ws_server.h"

#include "sylar/log.h"

namespace sylar
{
namespace http
{

static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

/**
 * @brief WSServer 构造函数
 *
 * @param worker 负责处理客户端连接的 IOManager
 * @param accept_worker 负责 accept 新连接的 IOManager
 */
WSServer::WSServer(sylar::IOManager *worker, sylar::IOManager *accept_worker)
    // 调用 TcpServer 构造函数。
    //
    // 因为 WSServer 继承 TcpServer，
    // 所以 WSServer 本身不重新实现监听、绑定端口、accept 这些通用 TCP 服务器逻辑。
    //
    // TcpServer(worker, accept_worker) 会保存：
    // 1. worker：处理连接用
    // 2. accept_worker：接收连接用
    : TcpServer(worker, accept_worker)
{
    // 初始化默认的 WebSocket servlet 分发器。
    //
    // 如果用户没有手动 setWSServletDispatch()，
    // WSServer 至少也有一个默认 dispatch 可用。
    //
    // 后续用户一般会这样注册处理器：
    //
    // server->getWSServletDispatch()->addServlet("/chat", ...);
    m_dispatch.reset(new WSServletDispatch);
}

/**
 * @brief 处理一个 WebSocket 客户端连接
 *
 * 这个函数是整个 WSServer 的核心。
 *
 * 调用来源：
 *
 * TcpServer accept 到一个新连接
 *          ↓
 * TcpServer 把 client socket 交给 handleClient()
 *          ↓
 * WSServer::handleClient(client)
 *
 * @param client 已经 accept 成功的客户端 socket
 */
void WSServer::handleClient(Socket::ptr client)
{

    SYLAR_LOG_DEBUG(g_logger) << "handleClient " << *client;

    // 把普通 Socket 包装成 WebSocket Session。
    //
    // Socket 只知道 send/recv 字节流；
    // WSSession 在 Socket 上增加了 WebSocket 协议处理能力：
    //
    // 1. HTTP Upgrade 握手
    // 2. WebSocket frame 解析
    // 3. ping/pong
    // 4. sendMessage/recvMessage
    WSSession::ptr session(new WSSession(client));

    // 使用 do while(0) 是 C/C++ 里常见的流程控制写法。
    //
    // 这里并不是为了循环多次。
    // 它只执行一次。
    //
    // 好处是：
    // 中间任何一步失败，都可以直接 break，
    // 然后统一走到最后的 session->close()。
    do
    {
        // 1. WebSocket 握手
        // 握手成功后，返回客户端最初发来的 HttpRequest。

        HttpRequest::ptr header = session->handleShake();

        // 如果握手失败，说明这个连接不是合法 WebSocket 连接，
        // 或者读写 socket 失败。
        if (!header)
        {
            SYLAR_LOG_DEBUG(g_logger) << "handleShake error";
            break;
        }

        // 2. 根据 WebSocket 握手请求里的 path 找对应 servlet。
        WSServlet::ptr servlet = m_dispatch->getWSServlet(header->getPath());

        // 如果没有找到对应 servlet，说明服务端没有处理这个 path 的逻辑。
        //
        // 当前代码选择直接断开连接。
        //
        // 注意：
        // 这里没有返回 WebSocket close frame，也没有返回 HTTP 错误响应。
        // 因为握手已经完成了，后面直接 break 关闭 session。
        if (!servlet)
        {
            SYLAR_LOG_DEBUG(g_logger) << "no match WSServlet";
            break;
        }

        // 3. 连接建立后的回调
        //
        // onConnect() 是 WebSocket 生命周期里的第一步业务回调。
        //
        // 典型用途：
        //
        // 1. 认证用户身份
        // 2. 初始化连接上下文
        // 3. 加入某个房间
        // 4. 记录在线状态
        //
        // 返回值约定：
        // 0 表示成功
        // 非 0 表示失败，需要断开连接
        int rt = servlet->onConnect(header, session);

        // 如果 onConnect 返回非 0，说明业务层拒绝这个连接。
        //
        // 例如：
        // 1. token 无效
        // 2. 用户无权限
        // 3. 连接数超过限制
        if (rt)
        {
            SYLAR_LOG_DEBUG(g_logger) << "onConnect return " << rt;
            break;
        }

        // 4. WebSocket 消息循环
        //
        // 握手成功后，WebSocket 不再是一次请求一次响应。
        //
        // 连接会一直存在，服务端不断读取客户端发来的 WebSocket frame。
        while (true)
        {
            // 从 WebSocket 连接中读取一条完整消息。
            //
            // recvMessage() 不是简单 recv 字节。
            // 它内部会解析 WebSocket frame：
            //
            // 1. 读取 frame header
            // 2. 判断 opcode
            // 3. 处理 payload length
            // 4. 如果客户端消息带 mask，需要解 mask
            // 5. 如果是分片消息，需要拼接
            // 6. 如果是 ping，可能自动回 pong
            // 7. 最终返回 WSFrameMessage
            auto msg = session->recvMessage();

            if (!msg)
            {
                break;
            }

            // 把收到的 WebSocket 消息交给业务 servlet 处理。
            rt = servlet->handle(header, msg, session);

            if (rt)
            {
                SYLAR_LOG_DEBUG(g_logger) << "handle return " << rt;
                break;
            }
        }

        // 5. WebSocket 连接关闭前的回调

        servlet->onClose(header, session);

    } while (0);

    // 统一关闭 session。
    session->close();
}

} // namespace http
} // namespace sylar