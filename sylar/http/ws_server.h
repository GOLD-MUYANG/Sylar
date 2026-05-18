#ifndef __SYLAR_HTTP_WS_SERVER_H__
#define __SYLAR_HTTP_WS_SERVER_H__

#include "sylar/tcp_server.h"

#include "ws_servlet.h"

#include "ws_session.h"

namespace sylar
{
namespace http
{

/**
 * @brief WebSocket 服务器类
 *
 * WSServer 继承自 TcpServer。
 *
 * TcpServer 负责：
 * 1. 监听端口
 * 2. accept 新连接
 * 3. 把新连接交给 handleClient()
 *
 * WSServer 负责：
 * 1. 把普通 TCP 连接包装成 WSSession
 * 2. 执行 WebSocket 握手
 * 3. 根据请求 path 找到对应 WSServlet
 * 4. 循环读取 WebSocket 消息
 * 5. 把消息交给 servlet 处理
 */
class WSServer : public TcpServer
{
public:
    // Sylar 项目里大量使用 ptr 作为 shared_ptr 类型别名。
    // 这样外部可以写 WSServer::ptr server(new WSServer);
    typedef std::shared_ptr<WSServer> ptr;

    /**
     * @brief 构造 WebSocket 服务器
     *
     * @param worker 处理客户端连接的 IOManager
     *        也就是 handleClient() 以及后续读写消息运行在哪个调度器上。
     *
     * @param accept_worker 负责 accept 新连接的 IOManager
     *        也就是监听 socket 上有新连接时，由哪个调度器负责 accept。
     *
     * 默认参数都是 sylar::IOManager::GetThis()。
     *
     * 这意味着：
     * 如果你没有显式传 worker / accept_worker，
     * 那么当前线程所在的 IOManager 同时负责 accept 和处理客户端。
     */
    WSServer(sylar::IOManager *worker = sylar::IOManager::GetThis(),
             sylar::IOManager *accept_worker = sylar::IOManager::GetThis());

    /**
     * @brief 获取 WebSocket servlet 分发器
     *
     * WSServletDispatch 的作用类似 HTTP ServletDispatch：
     *
     * 不同 path 对应不同处理逻辑，例如：
     *
     * /chat    -> 聊天 servlet
     * /echo    -> 回显 servlet
     * /notify  -> 通知 servlet
     *
     * @return 当前服务器使用的 WSServletDispatch
     */
    WSServletDispatch::ptr getWSServletDispatch() const
    {
        return m_dispatch;
    }

    /**
     * @brief 设置 WebSocket servlet 分发器
     *
     * 外部可以自定义一个 WSServletDispatch，然后塞给 WSServer。
     *
     * @param v 新的 WebSocket servlet 分发器
     */
    void setWSServletDispatch(WSServletDispatch::ptr v)
    {
        m_dispatch = v;
    }

protected:
    /**
     * @brief 处理一个客户端连接
     *
     * TcpServer accept 到一个 client socket 后，会调用 handleClient(client)。
     *
     * 对普通 TcpServer 来说，这里可以随便写业务协议。
     * 对 WSServer 来说，这里固定执行 WebSocket 协议流程：
     *
     * 1. 创建 WSSession
     * 2. 执行 WebSocket 握手
     * 3. 根据 path 找 WSServlet
     * 4. 调用 servlet->onConnect()
     * 5. 循环 recvMessage()
     * 6. 每收到一条消息，调用 servlet->handle()
     * 7. 连接结束时调用 servlet->onClose()
     * 8. 关闭 session
     *
     * override 表示重写 TcpServer 的虚函数。
     */
    virtual void handleClient(Socket::ptr client) override;

protected:
    /**
     * @brief WebSocket servlet 分发器
     *
     * 它负责根据 HTTP 握手请求里的 path 找对应的 WSServlet。
     *
     * 例如客户端握手请求：
     *
     * GET /chat HTTP/1.1
     * Upgrade: websocket
     *
     * 那么 header->getPath() 就是 "/chat"。
     * m_dispatch->getWSServlet("/chat") 会找对应的处理器。
     */
    WSServletDispatch::ptr m_dispatch;
};

} // namespace http
} // namespace sylar

#endif