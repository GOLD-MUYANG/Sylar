# Sylar RPC 模块任务书

## 1. 目标

基于现有 `sylar` HTTP 能力新增一套轻量级 RPC 模块，使项目从当前的网络协程框架扩展为可承载服务间调用的 RPC 框架。

RPC 模块第一阶段目标不是一次性做成完整微服务治理框架，而是先实现：

- 基于 HTTP 协议的 RPC 调用链路
- RPC Server
- RPC Client
- 请求/响应模型
- 方法注册与分发
- 同步调用接口
- 基础超时与错误处理
- 单元测试和简单示例

第一阶段不再设计自定义 TCP 二进制协议，不处理粘包、半包、固定包头和连接会话解析，把这些能力交给已有 HTTP 层承载。

## 2. 非目标

第一阶段暂不实现：

- 自定义 TCP RPC 协议
- 独立 RPC Session 层
- 服务注册中心
- 服务发现
- 负载均衡
- 熔断、限流、重试策略
- IDL 自动生成
- protobuf/thrift 深度集成
- JSON-RPC 2.0 完整兼容
- 分布式链路追踪
- 权限认证
- 跨语言 SDK

这些能力后续可以作为第二阶段、第三阶段继续演进。

## 3. 当前项目基础

现有项目已经具备 HTTP RPC 底座：

- `HttpServer`：底层仍然基于 `TcpServer`，但已经封装 HTTP 请求接收、解析和响应发送。
- `ServletDispatch`：可通过 `/rpc/*` 路由把 HTTP 请求转交给 RPC 分发器。
- `HttpRequest` / `HttpResponse`：可承载 RPC 方法名、请求 ID、错误码和业务 payload。
- `HttpConnection`：可用于客户端发送 HTTP POST 请求。
- `HttpConnectionPool`：后续可用于客户端连接复用。
- `IOManager` / `Scheduler` / `Fiber`：可承载 HTTP 服务端和客户端的协程调度。
- `WorkerManager`：可用于后续区分 IO worker 与业务 worker。
- `Config` / `Log`：可用于 RPC 配置和运行日志。
- `Module`：后续可扩展为动态加载 RPC 服务模块。

## 4. 模块目录规划

新增目录：

```text
sylar/rpc/
```

建议文件：

```text
sylar/rpc/rpc_message.h
sylar/rpc/rpc_message.cc
sylar/rpc/rpc_dispatcher.h
sylar/rpc/rpc_dispatcher.cc
sylar/rpc/rpc_server.h
sylar/rpc/rpc_server.cc
sylar/rpc/rpc_client.h
sylar/rpc/rpc_client.cc
```

不再新增：

```text
sylar/rpc/rpc_protocol.h
sylar/rpc/rpc_protocol.cc
sylar/rpc/rpc_session.h
sylar/rpc/rpc_session.cc
```

测试文件：

```text
tests/test_rpc_message.cc
tests/test_rpc_dispatcher.cc
tests/test_rpc_server.cc
tests/test_rpc_client.cc
```

示例文件：

```text
examples/rpc_server.cc
examples/rpc_client.cc
```

## 5. HTTP RPC 协议设计

第一阶段使用 HTTP POST 承载 RPC 调用。

推荐请求格式：

```http
POST /rpc/echo.echo HTTP/1.1
Host: 127.0.0.1:8020
Content-Type: application/octet-stream
X-Sylar-Rpc-Request-Id: 1
X-Sylar-Rpc-Timeout: 3000

hello
```

字段说明：

```text
HTTP method              固定使用 POST
HTTP path                /rpc/{service.method}
X-Sylar-Rpc-Request-Id   请求 ID，客户端生成，用于日志和响应匹配
X-Sylar-Rpc-Timeout      调用超时时间，单位毫秒，可选
HTTP body                业务请求 payload，第一阶段按原始字符串处理
```

推荐响应格式：

```http
HTTP/1.1 200 OK
Content-Type: application/octet-stream
X-Sylar-Rpc-Request-Id: 1
X-Sylar-Rpc-Code: 0
X-Sylar-Rpc-Message: OK

hello
```

字段说明：

```text
X-Sylar-Rpc-Request-Id   对应请求 ID
X-Sylar-Rpc-Code         RPC 业务错误码
X-Sylar-Rpc-Message      RPC 错误描述
HTTP body                业务响应 payload
```

HTTP 状态码只表达 HTTP 层状态：

```text
200 OK                   HTTP 请求合法，RPC 结果看 X-Sylar-Rpc-Code
400 Bad Request          RPC 请求路径、方法名或请求头非法
404 Not Found            未命中 /rpc/* 路由
405 Method Not Allowed   非 POST 请求
500 Internal Server Error 服务端内部异常
```

后续如果引入 JSON 库，可以把 body 升级为 JSON 或 JSON-RPC 2.0：

```json
{
  "id": 1,
  "method": "echo.echo",
  "params": "hello"
}
```

但第一阶段优先使用 HTTP path + header + 原始 body，避免引入额外 JSON 依赖。

## 6. 核心组件职责

### RpcMessage

负责 RPC 请求/响应的结构表达，以及 HTTP 请求/响应之间的转换。

职责：

- 保存 `request_id`
- 保存 `method`
- 保存 `code`
- 保存 `message`
- 保存 `payload`
- 从 `HttpRequest` 解析 RPC 请求
- 将 RPC 响应写入 `HttpResponse`
- 校验 `/rpc/{service.method}` 路径
- 校验请求方法必须是 POST
- 校验请求 ID、payload 长度等基础字段

### RpcDispatcher

负责服务方法注册和请求分发。

职责：

- 注册服务方法：`service.method -> callback`
- 根据请求方法名查找对应 callback
- 执行业务 callback
- 生成 RPC 响应
- 处理方法不存在、执行失败等错误

### RpcServer

负责把 RPC 分发器接入 HTTP 服务。

第一阶段建议组合 `http::HttpServer`，不重新实现 TCP 连接处理：

- 内部持有 `http::HttpServer::ptr`
- 在 `ServletDispatch` 中注册 `/rpc/*`
- 从 `HttpRequest` 解析 `RpcMessage`
- 调用 `RpcDispatcher`
- 将分发结果写入 `HttpResponse`
- 对外提供 `bind()`、`start()`、`stop()`、`registerService()` 等接口
- 记录调用日志、错误日志和耗时

### RpcClient

负责客户端同步调用。

第一阶段建议直接复用 `HttpConnection::DoPost()`：

- 保存 RPC 服务地址前缀，例如 `http://127.0.0.1:8020/rpc`
- 生成唯一 `request_id`
- 拼接调用地址：`{base_url}/{service.method}`
- 通过 HTTP POST 发送 payload
- 读取响应头中的 RPC 错误码和错误描述
- 返回响应 body 或错误状态
- 支持调用超时

后续可以基于 `HttpConnectionPool` 增加连接池和 keep-alive 复用。

## 7. 第一阶段接口草案

服务端注册接口：

```cpp
RpcServer::ptr server(new RpcServer);
server->registerService("echo.echo",
    [](const std::string& request, std::string& response) {
        response = request;
        return 0;
    });
```

服务端启动接口：

```cpp
RpcServer::ptr server(new RpcServer);
sylar::Address::ptr addr = sylar::Address::LookupAnyIPAddress("0.0.0.0:8020");
server->bind(addr);
server->registerService("echo.echo", echo_cb);
server->start();
```

客户端调用接口：

```cpp
RpcClient::ptr client(new RpcClient("http://127.0.0.1:8020/rpc"));

std::string response;
int rt = client->call("echo.echo", "hello", response, 3000);
```

## 8. 错误码规划

```text
0      OK
1001   请求路径非法
1002   请求方法非法
1003   请求 ID 非法
1004   请求体长度非法
1005   Content-Type 不支持
2001   服务方法不存在
2002   服务执行失败
3001   URL 非法
3002   解析 Host 失败
3003   连接失败
3004   发送失败
3005   接收失败
3006   调用超时
3007   响应格式非法
```

错误码通过 `X-Sylar-Rpc-Code` 返回。HTTP 层失败时，客户端需要把 `HttpResult::Error` 映射到 RPC 客户端错误码。

## 9. 实施阶段

### 阶段一：Message 层

任务：

- 新增 `RpcMessage`
- 定义 RPC 请求/响应字段
- 实现从 `HttpRequest` 解析请求
- 实现将 RPC 响应写入 `HttpResponse`
- 校验 HTTP method、path、request_id、payload 长度
- 增加消息层单元测试

验收：

- 能从 `/rpc/echo.echo` 解析出 `echo.echo`
- 能读取请求 body 作为 payload
- 能写出 `X-Sylar-Rpc-Code`、`X-Sylar-Rpc-Message`、响应 body
- 非 POST 请求能返回明确错误
- 非 `/rpc/*` 路径能返回明确错误

### 阶段二：Dispatcher 层

任务：

- 新增 `RpcDispatcher`
- 支持注册方法
- 支持方法查找
- 支持请求分发和响应生成
- 处理方法不存在和业务执行失败

验收：

- 能注册 `echo.echo`
- 能正常执行 callback
- 未注册方法返回 `2001`
- callback 返回非 0 时返回 `2002`

### 阶段三：Server 层

任务：

- 新增 `RpcServer`
- 内部组合 `http::HttpServer`
- 注册 `/rpc/*` servlet
- 在 servlet 中完成 `RpcMessage -> RpcDispatcher -> HttpResponse`
- 记录请求方法、request_id、错误码和耗时

验收：

- 服务端能监听指定 HTTP 端口
- 服务端能注册 `echo.echo`
- HTTP POST `/rpc/echo.echo` 能收到正确响应
- 未注册方法能收到明确 RPC 错误码
- 非 POST 请求能收到明确错误

### 阶段四：Client 层

任务：

- 新增 `RpcClient`
- 支持配置 RPC base URL
- 支持同步 `call(method, request, response, timeout_ms)`
- 使用 `HttpConnection::DoPost()` 发送请求
- 解析响应头和响应 body
- 将 HTTP 客户端错误映射为 RPC 错误码

验收：

- 客户端能调用 `echo.echo`
- 服务端返回业务错误时客户端能拿到错误码和错误描述
- 服务端不可达时返回连接失败
- 超时时返回 `3006`
- 响应缺少 RPC 头时返回 `3007`

### 阶段五：示例和构建接入

任务：

- 新增 `examples/rpc_server.cc`
- 新增 `examples/rpc_client.cc`
- 修改 `CMakeLists.txt` 接入 RPC 源文件和测试目标

验收：

- `cmake --build build` 通过
- `./bin/rpc_server` 可启动
- `./bin/rpc_client` 可调用并输出响应

## 10. 测试要求

至少新增以下测试：

- `test_rpc_message`
  - 正常请求解析
  - 正常响应写入
  - 非 POST 请求
  - 非 `/rpc/*` 路径
  - request_id 非法

- `test_rpc_dispatcher`
  - 方法注册
  - 方法不存在
  - 正常 echo 调用
  - callback 返回错误

- `test_rpc_server`
  - 服务端启动
  - 正常 HTTP RPC 调用
  - 方法不存在
  - 非法请求方法

- `test_rpc_client`
  - URL 非法
  - 连接失败
  - 调用成功
  - 调用超时
  - 响应格式非法

## 11. 验证命令

源码修改后至少执行：

```bash
cmake --build build
```

新增测试目标后执行：

```bash
./bin/test_rpc_message
./bin/test_rpc_dispatcher
./bin/test_rpc_server
./bin/test_rpc_client
```

RPC 示例完成后执行：

```bash
./bin/rpc_server
./bin/rpc_client
```

HTTP RPC 第一阶段测试应优先走本地回环地址，不依赖外网。

## 12. 风险点

- 当前 CMake 只启用了少量测试，新增测试需要明确接入。
- 当前项目没有现成 JSON 库，第一阶段不要把 JSON 解析作为主链路前置条件。
- `HttpConnection::DoPost()` 每次调用会新建连接，性能优化放到连接池阶段。
- `service.method` 放在 URL path 中时，需要避免 `/`、空格等非法字符；第一阶段可限制方法名只包含字母、数字、下划线和点。
- RPC 错误信息放在 HTTP header 中时，应避免换行和过长内容。
- 服务端业务 callback 如果阻塞，会占用 worker 协程，后续需要业务 worker 隔离。
- HTTP body 大小受现有 HTTP parser 配置限制，需要在文档和错误码中说明。

## 13. 推荐第一版完成标准

第一版 RPC 模块完成后，应满足：

- 可以启动 HTTP RPC Server。
- 可以注册一个服务方法。
- Client 可以通过 HTTP POST 调用服务方法。
- Server 能返回正常响应或明确错误。
- 客户端能区分 HTTP 连接错误和 RPC 业务错误。
- 消息层、分发器、客户端、服务端有基础测试。
- 示例能展示一次完整 RPC 调用。

这份任务书建议先按“最小可用 HTTP RPC”执行，不急着做服务治理。等请求/响应链路稳定后，再继续扩展 JSON/JSON-RPC、连接池、异步调用、服务发现、负载均衡和 protobuf。
