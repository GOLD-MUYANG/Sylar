# Sylar 服务框架

## 介绍

> [!NOTE]
> 本项目是基于 Sylar 实现的一个 Linux 下的高性能服务器框架，包括日志系统，配置系统，协程模块，IO 模块，定时器，网络系统等功能。
>
> 当前项目在原有服务框架基础上，已经继续扩展出一条 HTTP 客户端治理链路：
> `HttpConnectionPool -> HttpRequestOptions -> HttpClient -> retry -> HttpLoadBalanceClient -> HttpConcurrencyLimiter -> HttpCircuitBreaker`。
> 这条链路进一步被用于 AI 模型调用网关：本地 Mock Provider 闭环已可离线演示负载均衡、故障转移、限流、熔断和状态观测；真实 Provider 路径也已经具备配置读取、环境变量 Key、OpenAI-compatible 非流式适配、错误分类、执行预算、状态页和手工 smoke 入口，但真实外部访问默认关闭，需要操作者显式提供本机 API Key 后手工验证。

<br>

## 1、日志系统

> [!NOTE]
> - **对应文件**
>   - [`sylar/log.h`](sylar/log.h)、[`sylar/log.cc`](sylar/log.cc)：日志器、输出器、格式化器及日志事件实现。
>
> ### 日志系统整体流程（简版）
>
> - **创建 Logger**
>   - Logger 会带着一套默认格式字符串。
>   - 这套格式被交给 `Formatter`，`Formatter` 会解析并生成一组 `FormatItem`。
>   - 之后实际的字符串拼接，就是这些 `FormatItem` 完成的。
>
> - **添加 Appender（输出端）**
>   - 可以加控制台输出、文件输出，或同时加。
>   - 每个 Appender 默认使用 Logger 自己的 Formatter。
>   - 也可以在创建 Appender 时传入新的 Formatter，让不同的 Appender 输出不同风格。
>
> - **日志真正打印的过程**
>   - 日志宏生成 `LogEvent`，由 `LogEventWrap` 在析构时触发打印。
>   - 打印会调用 `logger.log()`。
>   - Logger 依次调用所有 Appender 的 `log()`。
>   - Appender 使用自己的 `Formatter.format()`，由其中的 `FormatItem` 把日志内容拼好。
>   - 拼好的字符串写入控制台或文件。
>
> - 核心链路：**Logger → Appender → Formatter → FormatItem拼接字符串 → StringStream输出**



<br><br><br>

## 2、配置系统与日志系统整合流程

> [!NOTE]
> - **对应文件**
>   - [`sylar/config.h`](sylar/config.h)、[`sylar/config.cc`](sylar/config.cc)：配置项注册、YAML 加载和变更回调。
>   - [`sylar/log.h`](sylar/log.h)、[`sylar/log.cc`](sylar/log.cc)：日志配置的解析和生效。
>
> - **初始化日志配置项**
>   - 定义 `LogDefine` 类，作为日志配置（日志器、格式化器、输出器）的载体；
>   - 向配置系统注册 `logs` 配置项，默认值为空 `LogDefine` 实例；
>   - 为 `logs` 配置项绑定回调函数，用于配置变更时更新日志系统。
>
> - **加载并解析配置文件**
>   - 配置系统加载配置文件，将层级化配置 Key 展平，保留对应节点；
>   - 遍历展平后的配置项，匹配到 `logs` 配置项时，将其节点序列化为字符串；
>   - 将字符串反序列化为新的 `LogDefine` 实例，更新配置系统中 `logs` 对应的值。
>
> - **触发回调初始化日志系统**
>   - `logs` 配置项的值更新后，自动触发绑定的回调函数；
>   - 回调函数解析新的 `LogDefine` 实例，生成/更新日志器、格式化器、输出器；
>   - 将生成的日志组件整合，完成日志系统的初始化并生效。

## 3、线程库的封装，为日志系统和配置系统添加锁的处理

> [!NOTE]
> - **对应文件**
>   - [`sylar/mutex.h`](sylar/mutex.h)：互斥锁、读写锁、信号量等同步原语封装。
>   - [`sylar/thread.h`](sylar/thread.h)、[`sylar/thread.cc`](sylar/thread.cc)：线程对象和线程局部信息封装。
>
> - 这一部分没什么好说的，主要是用 Linux 提供的系统方法去封装了锁和多线程，在日志系统读取和修改 logger 或 appender 之前加上锁；在配置系统读取和写配置之前加上锁。

## 4、协程

> [!NOTE]
> - **对应文件**
>   - [`sylar/fiber.h`](sylar/fiber.h)、[`sylar/fiber.cc`](sylar/fiber.cc)：协程对象、上下文切换和状态管理。
>
> - 封装 ucontext 库，实现协程的上下文切换。
> - 执行传入的需要协程来执行的方法。

## 5、协程调度器模块

> [!NOTE]
> - **对应文件**
>   - [`sylar/scheduler.h`](sylar/scheduler.h)、[`sylar/scheduler.cc`](sylar/scheduler.cc)：任务队列、线程调度和协程执行循环。
>
> - **使用方法**
>   - 创建 Scheduler 对象，调用 `schedule()` 放入需要执行的任务，开启调度器 `start()`。调度器将自动开启线程去处理任务。最后关闭调度器 `stop()`。
>
> - **原理**
>   - n 对 m 的协程调度器，n 个线程，执行 m 个任务。每个线程会从任务队列里取出任务执行，当任务队列为空时，线程会阻塞等待新的任务到来。
>   - 难点在于如何让包含执行 main 函数的线程，也去执行任务队列里的任务。
>   - `m_rootFiber.reset(new Fiber(std::bind(&Scheduler::run, this), 0, true));`
>   - 关键是理解 scheduler 里的这一行代码：这里新建了一个协程去执行 `run()`。

## 6、IO调度、Timer、Hook模块

> [!NOTE]
> - **对应文件**
>   - [`sylar/iomanager.h`](sylar/iomanager.h)、[`sylar/iomanager.cc`](sylar/iomanager.cc)：基于 epoll 的事件调度。
>   - [`sylar/timer.h`](sylar/timer.h)、[`sylar/timer.cc`](sylar/timer.cc)：定时器管理。
>   - [`sylar/hook.h`](sylar/hook.h)、[`sylar/hook.cc`](sylar/hook.cc)：阻塞系统调用 Hook 和协程让出。
>
> - use_caller为ture的情况下：
>   - 创建iomanager，初始化scheduler(对当前的线程，协程进行初始化，并绑定run方法)，准备epoll，开启调度（satrt()）
>   - start (开线程，绑定run)
>   - 加入任务（schedule）
>   - iomanager析构，执行schduler的stop方法，Fiber如果判断还不能停止（有任务，有定时器，有活跃线程）,就执行Fiber::call，因为只有m_rootFiber
>   - run（）<br>
>     - 拿到当前线程可以执行的任务
>     - 拿到以后，当前线程更改上下文去执行任务
>     - 正常执行的，执行完返回了；阻塞的，挂起了，等待idle线程将执行完的任务将阻塞任务设置为reday后重新调度进去
>     - 触发回调，进入hook sleep
>   - sleep():<br>
>     - 当作任务加入到定时器中（addTimer）
>     - 当前任务在定时器任务的最前面，就得让idle线程来处理一下(不过得等idle线程起来（也就是任务都加完结果都是阻塞的）)
>     - （对于前面有个耗时长的任务，idle起不来，定时器任务没法执行了）
>     - sleep等待执行中，返回到run，继续加任务
>     - 需要立马执行的任务处理完毕，但是还没有满足停止的条件，那么进入IOManger::idle，
>   - idle():<br>
>     - 找过期的定时器任务，重新调度（schedule()）,
>     - 看被阻塞的任务是否可以运行了（来了读写事件），也重新调度
>     - 看其他socket事件，没有需要处理的，返回run
>     - run中，定时器任务已经调度进去了，现在定时器任务开始执行。
>     - 都执行完了，
>     - （注意idle是一个while，一直在调用，每次看完有没有新任务，有没有定时器到期，只是swapout，只有满足stopping（）条件，才会真正break掉，然后MainFunc才能走下一步，把idle的状态改成TERM，才能开始退出）
>   - idle又进去了，没任务，idle swapout，再次运行，没任务，判断idle方法返回，run返回
>   - 退出

## 7、address模块

> [!NOTE]
> - **对应文件**
>   - [`sylar/address.h`](sylar/address.h)、[`sylar/address.cc`](sylar/address.cc)：IPv4、IPv6、Unix 域地址及地址解析。
>
> - 对IPv4Address和IPv6Address还有UnixAddress进行封装,主要是方便创建，获得掩码等，还有解析地址，获得网卡地址等，偏向于繁琐而不是技巧。

## 8、socket模块

> [!NOTE]
> - **对应文件**
>   - [`sylar/socket.h`](sylar/socket.h)、[`sylar/socket.cc`](sylar/socket.cc)：TCP、UDP 和 SSL Socket 的创建、连接、收发与关闭。
>   - [`sylar/socket_stream.h`](sylar/socket_stream.h)、[`sylar/socket_stream.cc`](sylar/socket_stream.cc)：将 Socket 适配为流式读写接口。
>
> - 对socket进行封装，主要是方便创建，绑定，监听，接受，发送，接收等操作，基本上就是使用hook的函数去封装起来，方便调用

## 9、bytearray模块

> [!NOTE]
> - **对应文件**
>   - [`sylar/bytearray.h`](sylar/bytearray.h)、[`sylar/bytearray.cc`](sylar/bytearray.cc)：二进制序列化、反序列化和分段内存管理。
>
> - bytearray模块实现序列化和反序列化，主要是为了解决不同编译器，不同底层的机器对不同类型变量的存储方式不同导致的存放和读取内存上有偏差而导致的问题，从而导致数据的丢失或错误。
>
> - 主要实现方式就是封装出一个bytearray类，将数据全部通过此种方式去传输，读取的时候内部封装了对内存的直接操作，可控，所有的常用类型都是直接操作内存，这样就不存一个机器上存4字节，另一个机器取8字节这种问题了。

## 10、从hook到http_server

> [!NOTE]
> - **对应文件**
>   - [`sylar/hook.h`](sylar/hook.h)、[`sylar/socket.h`](sylar/socket.h)、[`sylar/bytearray.h`](sylar/bytearray.h)：非阻塞 I/O、连接收发和数据编解码基础。
>   - [`sylar/tcp_server.h`](sylar/tcp_server.h)、[`sylar/tcp_server.cc`](sylar/tcp_server.cc)：TCP 连接监听与接入。
>   - [`sylar/http/http_parser.h`](sylar/http/http_parser.h)、[`sylar/http/http_parser.cc`](sylar/http/http_parser.cc)：HTTP 报文解析。
>   - [`sylar/http/http_server.h`](sylar/http/http_server.h)、[`sylar/http/http_server.cc`](sylar/http/http_server.cc)：HTTP 请求处理服务端。
>
> - 做一个server总得来说就是socket的操作。那么现在为了更好地使用socket，我们要做到一些事情：
>   - socket原生方法是阻塞的，现在要改成默认非阻塞的（hook）
>   - socket的方法要用起来非常地啰嗦，所以想要使用起来简单一点，就封装起来（sokcet）
>   - 传输过程中因为会遇到大小端的问题，存取数据不便，所以要封装一个bytearray类，来序列化和反序列化数据。
>   - 然后就到了tcp_server,做一个基类，用来处理tcp连接
>   - 接下来是 http_parser，其实就是解析http请求，封装到一个类里
>   - 然后就到了http_server,做一个基类，用来处理http连接（也就是怎么接收socket，怎么解析，怎么返回消息这一套）

## 11、http_servlet

> [!NOTE]
> - **对应文件**
>   - [`sylar/http/servlet.h`](sylar/http/servlet.h)、[`sylar/http/servlet.cc`](sylar/http/servlet.cc)：Servlet、函数式 Servlet 和路由分发。
>
> - 类似于Java概念里的servlet
> - 写的时候添加一个servlet，传一个handle方法，参数要有req,resp,session（我们自己封装的，其实就是为了方便接收和转发）,表示接收到请求以后我们对请求做了什么处理。

## 12、http_connection

> [!NOTE]
> - **对应文件**
>   - [`sylar/http/http_connection.h`](sylar/http/http_connection.h)、[`sylar/http/http_connection.cc`](sylar/http/http_connection.cc)：HTTP 客户端连接、连接池和请求发送。
>   - [`sylar/uri.h`](sylar/uri.h)、[`sylar/uri.rl`](sylar/uri.rl)：URI 解析。
>
> - 第一个就是客户端接收和返回请求的封装
> - 然后是对URI的解析
> - 最后是封装了请求的方法，也就是直接做了get,post等方法的封装。

## websocket模块

> [!NOTE]
> - **对应文件**
>   - [`sylar/http/ws_server.h`](sylar/http/ws_server.h)、[`sylar/http/ws_server.cc`](sylar/http/ws_server.cc)：WebSocket 服务端握手和连接处理。
>   - [`sylar/http/ws_connection.h`](sylar/http/ws_connection.h)、[`sylar/http/ws_connection.cc`](sylar/http/ws_connection.cc)：WebSocket 帧收发。
>   - [`sylar/http/ws_servlet.h`](sylar/http/ws_servlet.h)、[`sylar/http/ws_servlet.cc`](sylar/http/ws_servlet.cc)：WebSocket Servlet 路由。
>
> - 实现了websocket协议，包括握手，发送消息，接收消息等操作

## ssl模块

> [!NOTE]
> - **对应文件**
>   - [`sylar/socket.h`](sylar/socket.h)、[`sylar/socket.cc`](sylar/socket.cc)：`SSLSocket` 封装 OpenSSL 的握手、证书加载和加密收发。
>   - [`sylar/tcp_server.h`](sylar/tcp_server.h)、[`sylar/tcp_server.cc`](sylar/tcp_server.cc)：以 SSL Socket 绑定监听地址并加载证书。
>   - [`sylar/application.h`](sylar/application.h)、[`sylar/application.cc`](sylar/application.cc)：读取服务 SSL 配置并初始化证书。
>
> - 基于 OpenSSL 提供 HTTPS/TLS 连接能力；连接池访问 `https` 时默认加载系统可信 CA、校验证书和目标主机名，并将目标主机写入 SNI。私有 CA 或关闭校验必须显式设置 `SSLSocket::ClientOptions`；服务端可在配置中启用 SSL 并指定证书和私钥。

## worker模块

> [!NOTE]
> - **对应文件**
>   - [`sylar/worker.h`](sylar/worker.h)、[`sylar/worker.cc`](sylar/worker.cc)：命名工作组、调度器创建和任务投递。
>
> - `WorkerGroup` 是依附既有 `Scheduler` 的并发受限任务组：最多同时运行 `batch_size` 个任务；任务异常会记录错误并归还名额。`WorkerManager` 则按名称创建、查询和停止 `IOManager`，两者不是同一层能力。

## 插件模块

> [!NOTE]
> - **对应文件**
>   - [`sylar/module.h`](sylar/module.h)、[`sylar/module.cc`](sylar/module.cc)：模块接口、模块扫描和模块生命周期管理。
>   - [`sylar/library.h`](sylar/library.h)、[`sylar/library.cc`](sylar/library.cc)：动态库加载、符号查找和模块实例创建。
>
> - 扫描配置目录中的动态库，通过 `CreateModule` 和 `DestoryModule` 完成插件模块的加载与销毁。加载后依次支持 `onLoad`、所有服务 bind 完成后的 `onServerReady`、服务启动后的 `onServerUp`，以及每条 TCP 连接前后的 `onConnect` / `onDisconnect`；退出清理时调用 `onUnload`。回调失败或抛异常只记录错误，不接管服务的连接资源。


# 系统篇

> [!NOTE]
> - **对应文件**
>   - [`sylar/application.h`](sylar/application.h)、[`sylar/application.cc`](sylar/application.cc)：应用初始化、服务配置解析与启动。
>   - [`sylar/env.h`](sylar/env.h)、[`sylar/env.cc`](sylar/env.cc)：命令行参数和运行环境管理。
>   - [`sylar/daemon.h`](sylar/daemon.h)、[`sylar/daemon.cc`](sylar/daemon.cc)：前台与守护进程启动。
>   - [`sylar/config.h`](sylar/config.h)、[`sylar/config.cc`](sylar/config.cc)：配置目录加载。
>
> - 系统模块主要是做了一些和操作系统相关的操作，守护进程，参数解析，读取配置文件，环境变量，文件操作等
>   - 在最后集合在一起，把一个服务应该有的先注册进去（命令行，加载方式，读配置文件，检测运行状态并保存相关信息到文件里，启动调度器）
> - 启动调度器，绑定配置文件中的那些http_server用于处理http连接，等待客户端连接，让调度器处理即可。


# 至此，Sylar的基础部分就实现了

# 基于Sylar的扩展
## 失败重试

> [!NOTE]
> - **对应文件**
>   - [`sylar/http/http_client.h`](sylar/http/http_client.h)、[`sylar/http/http_client.cc`](sylar/http/http_client.cc)：`HttpRetryOptions`、重试条件判断和重试间隔计算。
>   - [`sylar/http/http_request_options.h`](sylar/http/http_request_options.h)、[`sylar/http/http_request_options.cc`](sylar/http/http_request_options.cc)：单次请求的超时等选项。
>
> - HTTP 客户端根据请求方法、错误结果和重试参数判断是否重试，并在重试间隔后重新发起请求。

## 负载均衡

> [!NOTE]
> - **对应文件**
>   - [`sylar/http/http_load_balance_client.h`](sylar/http/http_load_balance_client.h)、[`sylar/http/http_load_balance_client.cc`](sylar/http/http_load_balance_client.cc)：多 Endpoint 客户端、连接池和负载均衡策略。
>
> - 支持轮询、随机、加权轮询和最少连接策略；可跳过不可用节点并执行健康检查。

## 限流保护

> [!NOTE]
> - **对应文件**
>   - [`sylar/http/http_concurrency_limiter.h`](sylar/http/http_concurrency_limiter.h)、[`sylar/http/http_concurrency_limiter.cc`](sylar/http/http_concurrency_limiter.cc)：全局、服务和 Endpoint 维度的并发与 QPS 限制。
>
> - 通过 RAII 守卫自动归还并发配额，并使用令牌桶限制 QPS。

## 熔断

> [!NOTE]
> - **对应文件**
>   - [`sylar/http/http_circuit_breaker.h`](sylar/http/http_circuit_breaker.h)、[`sylar/http/http_circuit_breaker.cc`](sylar/http/http_circuit_breaker.cc)：Endpoint 维度的熔断状态、失败统计和半开探测。
>   - [`sylar/http/http_load_balance_client.h`](sylar/http/http_load_balance_client.h)、[`sylar/http/http_load_balance_client.cc`](sylar/http/http_load_balance_client.cc)：熔断器接入和失败节点切换。
>
> - 根据连续失败次数或窗口失败率切换 CLOSED、OPEN、HALF_OPEN 状态，阻止故障节点继续承接请求。

## AI 模型调用网关

> [!NOTE]
> - **对应文件**
>   - [`modules/ai_gateway/ai_gateway_module.cc`](modules/ai_gateway/ai_gateway_module.cc)：动态模块入口，读取网关配置并注册路由。
>   - [`modules/ai_gateway/ai_gateway_protocol.h`](modules/ai_gateway/ai_gateway_protocol.h)、[`modules/ai_gateway/ai_gateway_protocol.cc`](modules/ai_gateway/ai_gateway_protocol.cc)：Chat Completions 非流式基础请求、响应和错误对象编解码。
>   - [`modules/ai_gateway/ai_gateway_servlet.h`](modules/ai_gateway/ai_gateway_servlet.h)、[`modules/ai_gateway/ai_gateway_servlet.cc`](modules/ai_gateway/ai_gateway_servlet.cc)：`POST /v1/chat/completions` 入口，请求校验、统一响应和 trace 头处理。
>   - [`modules/ai_gateway/ai_gateway_upstream.h`](modules/ai_gateway/ai_gateway_upstream.h)、[`modules/ai_gateway/ai_gateway_upstream.cc`](modules/ai_gateway/ai_gateway_upstream.cc)：把多个 Mock Provider 装配到 `HttpLoadBalanceClient`，接入 deadline、attempt budget、limiter、breaker 和健康检查。
>   - [`modules/ai_gateway/ai_gateway_status_servlet.h`](modules/ai_gateway/ai_gateway_status_servlet.h)、[`modules/ai_gateway/ai_gateway_status_servlet.cc`](modules/ai_gateway/ai_gateway_status_servlet.cc)：只读 `/internal/status` 状态接口。
>   - [`modules/ai_gateway/ai_gateway_demo.html`](modules/ai_gateway/ai_gateway_demo.html)、[`modules/ai_gateway/ai_gateway_demo_servlet.cc`](modules/ai_gateway/ai_gateway_demo_servlet.cc)：本地演示页面和静态页面 Servlet。
>   - [`examples/mock_model_provider.cc`](examples/mock_model_provider.cc)：本地模型提供者模拟服务，支持 `normal`、`slow`、`error` 模式。
>
> - 网关对外提供 OpenAI Chat Completions 的非流式基础形状，第一版只覆盖文本 `messages`、`model`、基础可选参数和 OpenAI 风格错误对象，不实现 SSE 流式输出、tools/function calling、多模态、账号、计费或 RAG。
> - 本地 Mock 闭环已经完成 G0-G5：可启动多个 Mock Provider，按策略分流；Provider 失败、超时、限流或熔断时按预算切换候选；所有候选不可用时返回结构化错误，不无限排队、不无限重试。
> - `/internal/status` 展示 provider 健康状态、熔断状态、in-flight、成功/失败/限流计数和最近失败原因。`/demo` 页面可以发送请求并展示本次请求的 timeline；普通响应体不暴露内部 provider 名、地址、凭据或堆栈。
> - 演示入口：
>
>   ```bash
>   cmake --build build --target mock_model_provider ai_gateway_module bin_sylar
>   ./scripts/demo_ai_gateway.sh
>   ./scripts/demo_ai_gateway.sh --serve
>   ```

## 真实 Provider 网关

> [!NOTE]
> - **对应文件**
>   - [`modules/ai_gateway/real_provider_gateway.h`](modules/ai_gateway/real_provider_gateway.h)、[`modules/ai_gateway/real_provider_gateway.cc`](modules/ai_gateway/real_provider_gateway.cc)：`ProviderCandidate`、逻辑模型路由、共享 `RequestExecutionBudget` 和 provider-aware executor。
>   - [`modules/ai_gateway/ai_provider_adapter.h`](modules/ai_gateway/ai_provider_adapter.h)、[`modules/ai_gateway/ai_provider_adapter.cc`](modules/ai_gateway/ai_provider_adapter.cc)：`AiProviderAdapter` 契约和 OpenAI-compatible 非流式适配器。
>   - [`modules/ai_gateway/real_provider_runtime.h`](modules/ai_gateway/real_provider_runtime.h)、[`modules/ai_gateway/real_provider_runtime.cc`](modules/ai_gateway/real_provider_runtime.cc)：运行时读取 `real_providers` 配置、校验 provider 字段、按 `api_key_env` 读取环境变量并构建状态输出。
>   - [`modules/ai_gateway/real_provider_smoke.h`](modules/ai_gateway/real_provider_smoke.h)、[`modules/ai_gateway/real_provider_smoke.cc`](modules/ai_gateway/real_provider_smoke.cc)：真实 Provider smoke 的受控执行入口。
>   - [`examples/ai_gateway_real_provider_smoke.cc`](examples/ai_gateway_real_provider_smoke.cc)、[`scripts/smoke_ai_gateway_real_provider.sh`](scripts/smoke_ai_gateway_real_provider.sh)、[`scripts/demo_ai_gateway_real_provider.sh`](scripts/demo_ai_gateway_real_provider.sh)：手工真实请求和页面演示脚本。
>
> - 真实 Provider 路径解决的是 HTTPS、凭据、供应商错误分类、逻辑模型路由、非幂等重试安全和 provider-aware 调用治理，不训练模型，也不把某家 Provider SDK 塞进 Servlet。
> - 配置文件只保存 `api_key_env`，真实 Key 只能来自当前进程环境变量；Key、Authorization、完整用户消息和 Provider 原始响应不能进入日志、状态接口、trace、错误响应或测试快照。
> - 同一个 `logical_model` 下的候选必须有兼容的 `compatibility_key`。故障转移只表示“尽量完成本次服务调用”，不保证不同 Provider 的输出、usage、价格或内容策略完全一致。
> - 对可能已经提交给 Provider 的请求，默认不盲目重试或切换，避免重复执行和重复计费。只有连接失败、写入前失败等能确认未提交的情况，才会在总 deadline 和总尝试预算内切换候选。
> - 真实外部访问默认关闭；默认测试、CI 和 Mock demo 都不依赖外网、真实 API Key 或计费账户。手工 smoke 需要显式 opt-in：
>
>   ```bash
>   export ARK_API_KEY='本机真实 Key'
>   export SYLAR_AI_GATEWAY_REAL_SMOKE=1
>   ./scripts/smoke_ai_gateway_real_provider.sh
>   ```
>
> - 真实 Provider 页面演示同样必须显式开启，并以 [`examples/ai_gateway_conf/ai_gateway.yml`](examples/ai_gateway_conf/ai_gateway.yml) 中的 enabled provider 和 `api_key_env` 为准：
>
>   ```bash
>   export API_KEY_DOUBAO='本机真实 Key'
>   export API_KEY_DEEPSEEK='本机真实 Key'
>   export SYLAR_AI_GATEWAY_REAL_DEMO=1
>   ./scripts/demo_ai_gateway_real_provider.sh
>   ```

## 稳定性修复与测试入口

> [!NOTE]
> - 已补齐的可靠性修复包括：`WorkerGroup` 并发名额初始化与异常回收、配置目录失败返回、连接池非法 URI 返回空指针契约、模块 load/unload 与 ready/up/connect/disconnect 生命周期、HTTPS 客户端默认 CA/主机名/SNI 校验、启动失败非零退出、CTest 单元测试入口。
> - 常用构建和回归命令：
>
>   ```bash
>   cmake -S . -B build
>   cmake --build build
>   ctest --test-dir build -L unit --output-on-failure
>   ctest --test-dir build -R ai_gateway --output-on-failure
>   ```
>
> - HTTP 客户端链路常用测试：
>
>   ```bash
>   ./bin/test_http_request_options
>   ./bin/test_http_client
>   ./bin/test_http_load_balance_client
>   ./bin/test_http_concurrency_limiter
>   ./bin/test_http_circuit_breaker
>   ./bin/test_http_connection
>   ```
>
> - AI 网关相关测试：
>
>   ```bash
>   ./bin/test_ai_gateway_route_registry
>   ./bin/test_ai_gateway_protocol
>   ./bin/test_ai_gateway_servlet
>   ./bin/test_ai_gateway_load_balance
>   ./bin/test_ai_gateway_status
>   ./bin/test_ai_gateway_demo_servlet
>   ./bin/test_ai_gateway_real_provider
>   ./bin/test_ai_gateway_real_runtime
>   ```
>
> - `test_http_connection` 会访问外网；真实 Provider smoke 和真实 Provider demo 需要显式环境变量，不属于默认离线验证。

## 压测记录与后续方向

> [!NOTE]
> - [`文档/压力测试.md`](文档/压力测试.md) 中记录过本地 ApacheBench 结果：短连接 `ab -n 50000 -c 100` 约 5448 QPS，长连接 `ab -k -n 50000 -c 100` 约 23650 QPS。该结果用于说明当时本机环境下的服务端基线，不等同于跨环境的性能承诺。
> - 后续更适合继续推进的方向包括服务发现、RPC 语义、服务级熔断、降级回调、指标导出、真实 Provider 的离线 HTTPS 夹具和更完整的观测指标。
> - 暂不宣称已实现完整微服务治理平台、完整 OpenAI API、流式 SSE、tools/function calling、多模态、用户级计费或 token 成本治理。
