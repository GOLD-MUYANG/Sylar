# Sylar 服务框架

## 介绍

> [!NOTE]
> 本项目是基于 Sylar 实现的一个 Linux 下的高性能服务器框架，包括日志系统，配置系统，协程模块，IO 模块，定时器，网络系统等功能。
>
> 当前项目在原有服务框架基础上，已经继续扩展出一条 HTTP 客户端治理链路：
> `HttpConnectionPool -> HttpRequestOptions -> HttpClient -> retry -> HttpLoadBalanceClient -> HttpConcurrencyLimiter -> HttpCircuitBreaker`。
> 这条链路进一步被用于 AI 模型调用网关：本地 Mock Provider 闭环已可离线演示负载均衡、故障转移、限流、熔断和状态观测；真实 Provider 路径也已经具备配置读取、环境变量 Key、OpenAI-compatible 非流式适配、错误分类、执行预算、状态页和手工 smoke 入口，但真实外部访问默认关闭，需要操作者显式提供本机 API Key 后手工验证。

<br>

因为readme是先用AI整理出来的，后面我有自己整理了一下项目，而且AI整理的太教条了，不适合人看，所以我直接把我自己整理的贴在这里，适合对项目有初步的认识。

# 1. 总览

- 服务配置
- 网络
- HTTP
- Mock AI 网关
- 真实 AI 网关

# 2. 服务端运行链路

所有的服务都是被 `Application` 加载进来的。

用到的核心能力：

## 2.1 配置系统封装

配置系统主要分两步：

1. `Lookup` 注册配置项，并给一个默认值。
2. 加载 YAML 时，按配置名找到已注册项，再把 YAML 里的值写进去。

### 最小例子

代码里先注册配置项：

```cpp
static auto g_port =
    sylar::Config::Lookup<int>("server.port", 8080, "server port");
```

YAML 里再覆盖配置值：

```yaml
server:
  port: 9000
```

### Lookup 和 YAML 加载的左右对照

<table>
  <tr>
    <th width="50%">Lookup：先把配置项注册进全局表</th>
    <th width="50%">LoadFromYaml：再用 YAML 更新配置值</th>
  </tr>
  <tr>
    <td>
      <strong>入口</strong><br>
      <code>Config::Lookup&lt;int&gt;("server.port", 8080, "server port")</code>
    </td>
    <td>
      <strong>入口</strong><br>
      <code>LoadFromYaml(root)</code>
    </td>
  </tr>
  <tr>
    <td>创建一个名为 <code>server.port</code> 的 <code>ConfigVar&lt;int&gt;</code>。</td>
    <td>找到名为 <code>server.port</code> 的配置项，并把 YAML 值写进去。</td>
  </tr>
  <tr>
    <td>
      <strong>关键流程</strong>
      <ol>
        <li><code>GetMutex()</code> 拿全局写锁。</li>
        <li><code>GetDatas()</code> 取全局配置 <code>map</code>。</li>
        <li><code>find("server.port")</code> 查是否已存在。</li>
        <li>没找到时，检查配置名是否合法。</li>
        <li>创建 <code>ConfigVar&lt;int&gt;</code>。</li>
        <li>写入默认值 <code>8080</code>。</li>
        <li>放入 <code>GetDatas()["server.port"]</code>。</li>
      </ol>
    </td>
    <td>
      <strong>关键流程</strong>
      <ol>
        <li><code>ListAllMember</code> 把 YAML 展平成 <code>server.port</code>。</li>
        <li><code>LookupBase("server.port")</code> 查全局配置表。</li>
        <li>找到刚才注册的 <code>ConfigVar&lt;int&gt;</code>。</li>
        <li><code>fromString("9000")</code> 做字符串反序列化。</li>
        <li><code>LexicalCast&lt;string, int&gt;()("9000")</code> 转成 <code>int</code>。</li>
        <li><code>setValue(9000)</code> 更新配置值。</li>
      </ol>
    </td>
  </tr>
  <tr>
    <td>
      <strong>结果</strong><br>
      <code>m_name = "server.port"</code><br>
      <code>m_description = "server port"</code><br>
      <code>m_val = 8080</code><br>
      最后 <code>return v</code>。
    </td>
    <td>
      <strong>结果</strong><br>
      <code>m_val</code> 从默认值 <code>8080</code> 变成 YAML 里的 <code>9000</code>。
    </td>
  </tr>
</table>

### 一句话关系

| 动作 | 作用 |
| --- | --- |
| `Lookup` | 负责“把坑占好”：声明这个配置项存在，并给默认值。 |
| YAML 加载 | 负责“往坑里填新值”：如果配置文件里有值，就覆盖默认值。 |
| `getValue` | 负责“业务代码取当前值”：拿到默认值或 YAML 覆盖后的值。 |

## 2.2 env 配置项

`Env` 主要负责把进程启动时的零散信息统一收口。

### 主要作用

| 能力 | 说明 |
| --- | --- |
| 命令行解析 | 在 `init` 里解析 `-s`、`-d`、`-c` 等启动参数。 |
| 路径记录 | 在 `init` 里记录程序路径、程序所在目录、当前启动目录。 |
| 参数管理 | 维护命令行参数表，方便后续模块按名称读取。 |
| 帮助信息 | 统一保存和输出命令行帮助说明。 |
| 环境封装 | 封装环境变量读取，以及配置目录等路径获取。 |

### 一句话理解

整体来看，`Env` 不是复杂业务模块，而是把启动参数、路径和环境变量这类零散方法聚合起来，给 `Application` 和其他模块使用。

## 2.3 Daemon

`Daemon` 实现“守护进程启动 + 子进程崩溃自动重启”。

| 关注点 | 说明 |
| --- | --- |
| 使用位置 | 在 `Application` 里直接使用。 |
| 启动方式 | 执行命令时，如果想用守护进程方式启动，需要在可执行程序后面加 `-d`。 |
| 核心效果 | 父进程负责守护，子进程负责真正运行服务；子进程异常退出后，父进程可以重新拉起。 |

## 2.4 Worker

`Worker` 主要分成两个部分：`WorkerGroup` 和 `WorkerManager`。两者都和任务执行有关，但控制的层次不同。

| 对象 | 控制内容 | 核心作用 |
| --- | --- | --- |
| `WorkerGroup` | 一批要投递给 `Scheduler` 的任务 | 限制一批任务的并发数量，并等待任务全部完成。 |
| `WorkerManager` | 一组按名称管理的 `IOManager` | 从配置中创建执行资源，供服务器按名称选择。 |

### WorkerGroup：控制一批任务怎么丢给 Scheduler

可以这样理解：有很多任务要交给某个 `Scheduler` 执行，但不希望它们一下子全部运行，而是最多同时运行 N 个。

| 点 | 说明 |
| --- | --- |
| 控制对象 | 一批要投递给 `Scheduler` 的任务。 |
| 并发控制 | 用 `batch_size` 初始化信号量 `sem`，限制最多同时运行多少个任务。 |
| 完成等待 | `batch_size` 本身也会保存下来，用于判断总任务数，等待全部任务释放时会用到。 |

### WorkerManager：从配置里组织 IOManager

`WorkerManager` 的主要作用是根据配置准备一批 `IOManager`。服务需要处理连接相关工作时，再按名称取出对应的执行资源。

常见用途包括：

- `accept`
- `read`
- `write`

### WorkerManager 在项目中的实际使用场景

#### 1. 配置 Worker

| 配置文件 | 作用 |
| --- | --- |
| `worker.yml` | 定义一批带名称的 `IOManager`，例如 `accept` 和 `io`。 |
| `server.yml` | 通过 `accept_worker` 和 `process_worker`，指定一个 `http_server` 使用哪些 Worker。 |

#### 2. Application 初始化并注入 Worker

`Application` 启动时，会让 `WorkerManager` 根据 `worker.yml` 初始化一批 `IOManager`。创建 `HttpServer` 时，再根据 `server.yml` 中的名称取出对应的 `IOManager`：

| Worker 名称 | 注入后的用途 |
| --- | --- |
| `accept_worker` | 负责运行 TCP 连接的 `accept` 循环。 |
| `process_worker` | 负责处理已经接收的客户端连接。 |

整体关系如下：

```text
worker.yml 定义 IOManager
        ↓
WorkerManager 初始化并按名称管理
        ↓
server.yml 为 HttpServer 指定 accept_worker / process_worker
        ↓
Application 取出对应 IOManager，并注入 HttpServer
```

#### 3. TcpServer 分配两类任务

启动 TCP 服务器时，把 `accept` 循环投递给 `accept_worker`：

```cpp
m_acceptWorker->schedule(
    std::bind(&TcpServer::startAccept, shared_from_this(), sock));
```

收到客户端连接后，再把连接处理任务转交给 `process_worker`：

```cpp
Socket::ptr client = sock->accept();

m_worker->schedule(
    std::bind(&TcpServer::handleClientWithModule,
              shared_from_this(),
              client));
```

这里的成员变量和配置名称对应关系是：

| `TcpServer` 成员变量 | 对应配置 | 负责的任务 |
| --- | --- | --- |
| `m_acceptWorker` | `accept_worker` | 监听并接收连接。 |
| `m_worker` | `process_worker` | 处理已经建立的连接。 |

#### 4. 进入 HttpServer 处理 HTTP 请求

连接处理任务最后通过虚函数进入 `HttpServer`，完成：

```text
接收 HTTP 请求
    ↓
ServletDispatch 路由
    ↓
执行业务 Servlet
    ↓
发送 HTTP 响应
    ↓
如果 keepalive，继续等待下一次请求
```

### 和连接池的边界

| 模块 | 控制的对象 | 是否为连接池 |
| --- | --- | --- |
| `WorkerManager` | `IOManager` / 执行资源 | 不是连接池。它管理的是执行资源，不是连接本身。 |
| HTTP/TCP 连接池 | HTTP/TCP 连接 | 是连接池。它解决连接复用问题，避免每次请求都重新建立连接。 |

一句话理解：`Worker` 解决的是“任务放到哪里执行”，连接池解决的是“连接怎么复用”。

## 2.5 Module

每个具体模块都应该继承 `Module`，再重写相关方法，实现自己的功能。

### Module 和 ModuleManager 的分工

| 对象 | 职责 |
| --- | --- |
| `Module` | 表示一个具体模块，提供生命周期方法。 |
| `ModuleManager` | 负责统一加载、保存、管理模块，并在固定时机回调模块方法。 |
| `Application` | 在自身启动流程里调用 `ModuleManager`，让模块跟着服务生命周期一起工作。 |

说白了就是：`Application` 执行到某个阶段时，如果模块有什么要补充的逻辑，就通过 `ModuleManager` 调用模块重写的方法。

### 加载流程

加载由 `ModuleManager` 实现，整体流程如下：

1. 指定 `.so` 存放位置。
2. 加载 `.so`，准备初始化模块。
3. 从模块里找到对外暴露的创建和销毁方法。
4. 调用创建方法，成功创建 `Module` 对象。
5. 调用模块的 `onLoad` 方法。
6. 把模块添加到 `ModuleManager` 中统一管理。


## 2.6 TcpServer

`TcpServer` 是上层协议服务的通用 TCP 底座。

上层协议服务，比如 `HttpServer`，可以继承 `TcpServer`，再重写 `handleClient()` 来处理自己的协议逻辑。

### 主要职责

| 职责 | 说明 |
| --- | --- |
| 绑定地址 | 负责 `bind` 服务器监听地址。 |
| 启动监听 | 进入监听状态，等待客户端连接。 |
| 接收连接 | 接收新连接，并把连接交给后续处理逻辑。 |
| 停止服务 | 提供停止服务器的统一入口。 |
| 通用处理入口 | 提供 `handleClient()`，让子类按具体协议重写。 |

### 一句话理解

`TcpServer` 只关心 TCP 服务怎么启动、监听、接收和停止；具体收到连接以后怎么解析协议、怎么处理业务，交给子类实现。

## 2.7 HttpServer

`HttpServer` 继承自 `TcpServer`，主要功能是处理 HTTP 请求。

它自己不直接写业务逻辑，而是把 HTTP 服务该做的通用流程封装好，再把具体业务交给 `ServletDispatch`。

### 处理流程

| 阶段 | HttpServer 做什么 |
| --- | --- |
| 接收请求 | 从连接中读取 HTTP 请求。 |
| 解析请求 | 解析请求行、请求头和请求体。 |
| 路由分发 | 把请求交给 `ServletDispatch`，由具体 `Servlet` 处理。 |
| 返回响应 | 组织 HTTP 响应格式，并写回客户端。 |

### 一句话理解

`HttpServer` 是 `TcpServer` 上的一层 HTTP 协议封装：TCP 连接由父类接进来，HTTP 解析、路由和响应由自己处理。

# 3. 协程模块

## 3.1 Scheduler

`Scheduler` 是协程任务的调度器。它解决的不是“任务具体做什么”，而是“任务放到哪里、由哪些线程执行、没任务时怎么等待、有任务时怎么唤醒、什么时候退出”。

### 为什么需要 Scheduler

| 问题 | Scheduler 负责的事情 |
| --- | --- |
| 开几个线程 | 启动指定数量的线程，让这些线程一起跑调度循环。 |
| 有哪些协程任务 | 把外部提交的任务统一放进任务队列。 |
| 任务怎么执行 | 工作线程从队列里取任务，切到对应协程运行。 |
| 没任务怎么办 | 进入 `idle()`，避免线程空转。 |
| 有新任务怎么办 | 通过 `tickle()` 唤醒空闲线程。 |
| 怎么退出 | `stop()` 触发退出，处理完已有任务后安全停止。 |

### 最小模型

本质上可以先把调度器理解成两个动作：

```cpp
add_task(task);  // 放任务，对应 Scheduler::schedule()
run();           // 取任务执行，对应 Scheduler::run()
```

在 Sylar 里，`schedule()` 会在具体场景中提交任务；`run()` 才是这个类的核心执行循环。

### 从 start 开始看

理解 `Scheduler` 时，可以先从 `start()` 入手：

1. 创建指定数量的线程。
2. 每个线程都执行 `Scheduler::run()`。
3. `run()` 不断从任务队列中取任务执行。
4. 收到 `stop()` 后，不再接收新的调度工作。
5. 当前已有任务处理完后，调度器安全退出。

### run 核心流程

```cpp
while (true)
{
    从 m_fibers 里找当前线程能执行的任务；

    如果找到 Fiber，就 swapIn();
    如果找到 cb，就包装成 Fiber 再 swapIn();
    如果没任务，就进入 idle();
    如果满足停止条件，就退出；
}
```

### idle 和 tickle

| 方法 | 作用 |
| --- | --- |
| `idle()` | 没任务时执行。意思是当前线程暂时没活干，不能一直空转，所以先把协程挂起来。具体等待逻辑由子类实现。 |
| `tickle()` | 有新任务或重新满足执行条件时执行。作用是把空闲线程从 `idle` 状态叫醒，让它回到 `run` 循环继续取任务。 |

### 快速索引

| 问题 | 对应实现 |
| --- | --- |
| 外部怎么提交任务？ | `schedule()` |
| 任务放到哪里？ | `m_fibers` |
| 任务长什么样？ | `FiberAndThread` |
| 谁来执行任务？ | `start()` 创建线程，线程跑 `run()` |
| `run()` 没任务怎么办？ | `idle()` |
| 有新任务怎么叫醒 `idle` 线程？ | `tickle()` |
| 什么时候能退出？ | `stopping()` |
| 当前线程也参与调度怎么办？ | `m_rootThread` + `m_rootFiber` |
| 当前协程要换调度器怎么办？ | `switchTo()` |

至于为什么任务能执行，然后还能跳转回来，是由底层的 `Fiber` 实现的。


## 3.2 IOManager

`IOManager` 是在 `Scheduler` 基础上加了一层 `epoll` + 定时器。

`Scheduler` 能调度普通任务：

```cpp
schedule([] {
    // 普通任务
});
```

但网络服务器里大量场景不是“马上能执行”，而是“先等条件满足”：

| 等待场景 | 如果直接阻塞会怎样 |
| --- | --- |
| `socket` 当前读不到数据 | 当前线程被卡住，不能继续跑其他协程。 |
| `socket` 当前写不进去 | 当前线程被卡住，写就绪前无法复用执行能力。 |
| 定时器还没到时间 | 当前任务只能等待，不能一直占住线程。 |

所以 `IOManager` 解决的是：**阻塞 IO 如何变成协程级别的非阻塞等待**。

### 协程遇到 IO 阻塞时发生什么

```text
协程遇到 IO 阻塞
-> 把 fd 和当前协程注册到 epoll
-> 当前协程让出执行权
-> 线程继续执行别的协程
-> fd 就绪后，epoll_wait 返回
-> IOManager 把原来的协程重新 schedule()
-> 协程继续执行
```

### epoll 和 FdContext 的分工

| 对象 | 负责什么 |
| --- | --- |
| `epoll` | 负责等待 fd 事件，只认识 fd 和事件类型。 |
| `FdContext` | 负责记住 fd 背后对应的协程、回调和事件状态。 |
| `Scheduler` | 负责在事件触发后重新调度对应协程或回调。 |

也就是说，`epoll` 的事件状态和 `IOManager` 自己保存的协程状态必须保持一致。

### 对 epoll 操作时要处理的复杂点

| 问题 | 为什么会复杂 |
| --- | --- |
| 一个 fd 可能同时监听 `READ` / `WRITE` | 添加或删除一个事件时，不能误伤另一个事件。 |
| `epoll` 只认识 fd | 需要 `FdContext` 把 fd 映射回协程或回调。 |
| IO 事件触发后要回到调度器 | 事件不是直接执行业务，而是要重新 `schedule()`。 |
| 删除和取消语义不同 | 删除只是移除监听；取消通常还要触发对应事件。 |
| `epoll_wait` 可能正在阻塞 | 普通任务和定时器变化时，需要通过 `tickle` 管道唤醒它。 |
| 多线程会同时操作事件状态 | `add` / `del` / `cancel` 不能把 `FdContext` 状态改乱。 |

所以对 `epoll` 的增、删、取消、触发，都要同步处理线程、协程和回调状态。

### idle 核心流程

`IOManager` 的 `idle()` 不只是“没任务就睡眠”，它还负责等待 IO 事件和定时器：

1. 检查当前是否可以停止。
2. 计算最近一个定时器的超时时间。
3. 调用 `epoll_wait()` 等待事件。
4. 处理 `tickle` 管道事件。
5. 处理真正的 fd 读写事件。
6. 把触发的协程或回调重新 `schedule()`。
7. 让出 `idle` 协程，回到 `Scheduler::run()`。

一句话理解：`Scheduler` 负责“任务怎么跑”，`IOManager` 负责“IO 没准备好时先挂起，准备好后再把任务放回调度器”。

## 3.3 定时器

定时器相关逻辑可以分成三层：

| 对象 | 主要职责 |
| --- | --- |
| `Timer` | 保存“什么时候触发、触发什么、是否重复”。 |
| `TimerManager` | 按触发时间管理所有 `Timer`，找出已经到期的回调。 |
| `IOManager` | 用 `epoll_wait()` 的 `timeout` 等待定时器到期，再把到期回调交给 `Scheduler` 执行。 |

### 整体关系

1. `Timer` 记录触发时间和回调。
2. `TimerManager` 统一组织这些定时器。
3. `IOManager` 根据最近一个定时器计算等待时间。
4. 定时器到期后，对应回调重新进入 `Scheduler` 执行。

一句话理解：`Timer` 描述单个定时任务，`TimerManager` 管理所有定时任务，`IOManager` 负责等待它们到期并交回调度器。

## 3.4 统筹理解 Scheduler、IOManager、TimerManager

普通服务器程序经常要做三种事：

| 任务类型 | 例子 |
| --- | --- |
| 执行计算任务 | 运行普通函数或协程任务。 |
| 等待网络事件 | 等待 socket 可读、可写。 |
| 等待时间到达 | 等待定时器超时。 |

问题是，等待网络和等待时间都可能阻塞线程。

Sylar 的思路是：

1. 线程不阻塞在某一个任务上。
2. 任务需要等待时，先挂起当前协程。
3. 线程继续执行其他可运行的协程。

```text
┌──────────────────────────────┐
│        Scheduler 任务队列     │
└──────────────┬───────────────┘
               │
               ▼
        有任务就执行任务
               │
               ▼
        任务执行到等待点
         /             \
        /               \
等待网络 IO           等待时间
        \               /
         \             /
          ▼           ▼
           IOManager
       epoll_wait(timeout)
               │
               ▼
      IO 就绪或定时器到期
               │
               ▼
      重新加入 Scheduler
```

### 三者分工

| 模块 | 解决的问题 |
| --- | --- |
| `Scheduler` | 管理可运行的任务，并把任务交给线程执行。 |
| `IOManager` | 等待 IO 事件，同时通过 `epoll_wait(timeout)` 兼顾最近的定时器。 |
| `TimerManager` | 管理定时器，并找出已经到期的回调。 |

一句话理解：任务能运行时由 `Scheduler` 执行；任务需要等待 IO 或时间时由 `IOManager` 等待；等待结束后再回到 `Scheduler`。

## 3.5 Hook

普通的 `sleep()`、`recv()` 等系统调用并不知道协程和调度器的存在。一旦直接调用这些阻塞函数，整个线程都会被阻塞。

Hook 的作用就是拦截这些系统调用：需要等待时不阻塞线程，而是挂起当前协程，交给 `IOManager` 或 `TimerManager` 等待。

### 文件结构

`hook.cc` 可以分成五个部分：

```text
Hook
├── 1. 初始化和原函数指针
├── 2. sleep 系列
├── 3. socket/connect/accept
├── 4. read/write/recv/send 通用处理
└── 5. close/fcntl/ioctl/setsockopt 状态维护
```

### 为什么要保存原函数指针

项目定义了与系统调用同名的函数，但实际执行时仍然需要调用 libc 中的原始函数。否则，自定义函数会不断调用自己，形成无限递归。

以 `sleep()` 为例，最终形成两个入口：

| 入口 | 含义 |
| --- | --- |
| `sleep()` | 项目自己实现的协程版本。 |
| `sleep_f()` | libc 原来的阻塞版本。 |

代码里的关键步骤如下：

1. 定义原函数的函数指针类型。

   ```cpp
   typedef unsigned int (*sleep_fun)(unsigned int seconds);
   ```

2. 声明函数指针变量。`extern` 表示变量存在，但定义和值在其他位置。

   ```cpp
   extern sleep_fun sleep_f;
   ```

3. 用 `dlsym()` 找到 libc 中原始 `sleep()` 的地址。

   ```cpp
   sleep_f = (sleep_fun)dlsym(RTLD_NEXT, "sleep");
   ```

不能直接写成：

```cpp
sleep_f = &sleep;
```

因为当前作用域中的 `sleep` 已经是项目自己定义的版本，而不是 libc 的原始版本。

### `do_io()`：Hook 的核心模板

#### 第一阶段：判断是否需要协程化

遇到以下情况时，不进入协程等待流程：

1. Hook 没有开启。
2. `FdManager` 中没有这个 fd。
3. fd 已经关闭。
4. fd 不是 socket。
5. 用户主动设置了非阻塞。

#### 第二阶段：先调用一次原始函数

fd 可能已经就绪，因此 `do_io()` 会先尝试直接执行：

| 结果 | 处理方式 |
| --- | --- |
| 调用成功 | 直接返回结果。 |
| `EINTR` | 说明系统调用被信号打断，立即重试。 |
| `EAGAIN` | 当前暂时无法完成，Hook 开始接管等待流程。 |
| 其他错误 | 直接返回错误。 |

### 把 `recv()` 的完整链路连起来

```text
业务代码调用 recv()
        ↓
进入自定义 recv()
        ↓
do_io(fd, recv_f, READ, SO_RCVTIMEO)
        ↓
调用原始 recv_f()
        ↓
┌──────────────────────┐
│ 成功                  │ → 直接返回数据
├──────────────────────┤
│ EINTR                 │ → 立即重试
├──────────────────────┤
│ 其他错误              │ → 直接返回错误
├──────────────────────┤
│ EAGAIN                │
└──────────┬───────────┘
           ↓
注册 READ 事件
注册超时 Timer
           ↓
当前 Fiber Yield
           ↓
┌──────────────────────┐
│ fd 可读               │ → 恢复协程 → 取消 Timer → 重试 recv_f
├──────────────────────┤
│ Timer 到期            │ → cancelEvent → 恢复协程 → 返回超时
└──────────────────────┘
```

一句话理解：Hook 把“阻塞线程等待系统调用”改成“挂起当前协程，事件就绪或超时后再恢复执行”。

## 3.6 Socket

裸 Socket API 是一组零散的过程，`Socket` 类把这些过程封装成对象，统一管理连接。

### 主要作用

| 作用 | 说明 |
| --- | --- |
| 管理 fd 生命周期 | 统一负责 socket 的创建、使用和关闭。 |
| 隐藏地址族差异 | 把 IPv4、IPv6、Unix 域等差异封装在统一接口后面。 |
| 接入协程 IO | 和 Hook、`FdManager`、`IOManager` 串联起来。 |
| 统一上层接口 | 让普通 TCP 和 TLS 共用一套上层使用方式。 |

### 普通服务端使用流程

服务端使用过程可以压缩成：

```cpp
Address::ptr addr = ...;

Socket::ptr sock = Socket::CreateTCP(addr);

sock->bind(addr);
sock->listen();

while (true)
{
    Socket::ptr client = sock->accept();

    client->recv(...);
    client->send(...);
}
```

### TCP 和 TLS 的关系

HTTPS 不是另一种底层网络协议。它仍然先建立 TCP，然后在 TCP 上进行 TLS 握手。

| 对象 | 作用 |
| --- | --- |
| `SSL_CTX` | 保存可供多条连接复用的公共 TLS 配置。 |
| `SSL` | 表示一条具体连接的 TLS 会话。 |

服务端常见结构如下：

```text
一个 SSL_CTX
├── 客户端连接 SSL 1
├── 客户端连接 SSL 2
└── 客户端连接 SSL 3
```

### 客户端 TLS 流程

```cpp
Socket::connect(...);
SSL_CTX_new(...);
SSL_new(...);
SSL_set_fd(...);
SSL_connect(...);
```

对应的理解顺序是：

1. 先连上 TCP。
2. 创建 TLS 配置。
3. 创建本次 TLS 会话。
4. 绑定 TCP fd。
5. 开始客户端握手。

### 服务端 TLS 流程

```cpp
accept(...);
SSL_new(...);
SSL_set_fd(...);
SSL_accept(...);
```

对应的理解顺序是：

1. 先接受 TCP 客户端。
2. 为它创建独立的 TLS 会话。
3. 绑定客户端 fd。
4. 执行服务端握手。

一句话理解：`Socket` 统一封装底层网络操作；TLS 在 TCP 连接之上增加握手和加密，每条连接使用独立的 `SSL` 会话。

# 4. HTTP 应用层

## 4.1 连接池

这一层同时处理发送前和发送后的错误。实际请求涉及两个对象：

| 对象 | 职责 |
| --- | --- |
| `HttpConnection` | 表示一条 TCP 连接；HTTPS 下则表示一条已建立 TLS 会话的连接。 |
| `HttpConnectionPool` | 管理一批连向同一目标的 TCP（SSL）连接，负责按需创建和复用。 |

### 请求入口

`DoGet()` 和 `DoPost()` 最终都会收敛到：

```cpp
HttpConnection::DoRequest(...);
```

这个过程会获取请求连接，检查请求格式，设置请求字段，并判断地址、Socket fd 和 SSL 状态是否合法。保存请求状态后，再调用 `sendRequest()` 开始发送。

### `sendRequest()` 发送流程

```text
HttpRequest
    ↓ operator<< 序列化
HTTP 请求字符串
    ↓ 循环 write() 到 Socket，同时更新 HttpAttemptOutcome
Socket
```

`HttpAttemptOutcome` 会作为后续判断重试安全性的依据。

### `recvResponse()` 响应流程

`recvResponse()` 先循环读取并解析响应头：

```text
Socket::recv
    ↓
临时 buffer
    ↓
HttpResponseParser::execute
    ↓
响应头解析完成
```

随后根据 HTTP 版本和 `Connection` 信息设置：

```cpp
response->setClose(true / false);
```

连接池会据此决定该连接能否继续复用。

### 连接池复用流程

`HttpConnectionPool` 不会提前建立全部连接，而是在请求到来时按需创建。连接使用完后会被放回池中，等待后续请求复用，避免每次都重新建立 TCP 连接。

`getConnection()` 的主要流程是：

```text
检查空闲连接
    ├─ 找到可复用连接 → 返回
    ├─ 找到失效连接 → 删除并减少 m_total
    └─ 没有空闲连接
          ├─ 未达到 maxSize → 创建新连接
          └─ 已达到 maxSize → 当前协程挂起等待
                                  ├─ 连接归还后唤醒
                                  └─ 等待超时返回 nullptr
```

池满后的等待不会阻塞整个线程，而是让当前协程挂起：

```cpp
Fiber::YieldToHold();
```

### 一句话理解

`HttpConnection` 负责一条连接上的请求与响应；`HttpConnectionPool` 负责同一目标的连接创建、等待和复用。

## 4.2 单实例客户端

`HttpClient` 是建立在 `HttpConnectionPool` 之上的“单上游实例业务客户端”，负责提供易用接口、错误归一化和基础重试；它自己不直接管理 `Socket`。

### 整体调用链

```text
业务代码
    ↓
HttpClient::Get/Post/Request
    ↓
统一进入 HttpClient::request()
    ↓
HttpConnectionPool::doRequest()
    ↓
获取/复用 HttpConnection
    ↓
Socket/TLS 发送请求并接收响应
    ↓
HttpClient::NormalizeResult()
    ↓
ShouldRetry() 判断是否重试
    ↓
返回 HttpResult
```

### 两套请求方式

| 方式 | 用法 | 特点 |
| --- | --- | --- |
| 静态接口 | `HttpClient::Get("http://example.com/a", 1000)` | 每次根据完整 URL 创建临时 `HttpClient` 和连接池。 |
| 对象接口 | 先用 `HttpClient::Create(...)` 绑定上游 | 后续使用相对路径发起请求。 |

对象接口的使用方式：

```cpp
auto client = HttpClient::Create("http://example.com");

auto r1 = client->get("/api/users", 1000);
auto r2 = client->post("/api/orders", 1000, {}, body);
```

### 多实例场景

多个上游实例由外层的 `HttpLoadBalanceClient` 统一组织：

```text
HttpLoadBalanceClient
  ├── endpoint A → HttpConnectionPool
  ├── endpoint B → HttpConnectionPool
  └── endpoint C → HttpConnectionPool
```

### 一句话理解

`HttpClient` 封装单个上游的请求、错误归一化和重试；多上游的选择则由 `HttpLoadBalanceClient` 负责。

## 4.3 负载均衡客户端

`HttpConnectionPool` 负责一个后端的连接复用；`HttpLoadBalanceClient` 负责从多个后端中选择一个，并串起限流、熔断、请求、故障切换和观测统计。

```text
HttpLoadBalanceClient
├── Endpoint A ── HttpConnectionPool A
├── Endpoint B ── HttpConnectionPool B
├── Endpoint C ── HttpConnectionPool C
├── HttpConcurrencyLimiter
└── HttpCircuitBreaker
```

| 对象 | 负责的问题 |
| --- | --- |
| `Endpoint` | 表示“哪个服务实例”。 |
| `HttpConnectionPool` | 管理“连接到这个实例的 socket”。 |
| `HttpLoadBalanceClient` | 决定“这次选哪个实例”。 |

### 完整请求链路

```text
规范化 path
    ↓
选择一个 UP 且本轮未尝试过的 Endpoint
    ↓
活跃请求数 +1
    ↓
限流器准入
    ↓
熔断器准入
    ↓
Endpoint 的连接池发送请求
    ↓
HttpClient::NormalizeResult 统一错误
    ↓
更新熔断器、Endpoint 统计和 Trace
    ↓
成功返回 / 切换其他 Endpoint / 进入下一轮重试
```

### 一句话理解

`HttpLoadBalanceClient` 站在多个单实例连接池之上，负责实例选择，并统一组织限流、熔断和故障切换。

## 4.4 限流器

请求发送前，限流器会检查全局、当前服务和当前 `Endpoint` 的并发数与 QPS 是否超限；超限就立即拒绝，不排队等待。

### 两类限制指标

| 维度 | 含义 | 请求结束后是否归还 |
| --- | --- | --- |
| `concurrency` | 当前正在执行多少请求 | 归还 |
| QPS | 一段时间内启动多少请求 | 不归还，随时间补充 |

并发限制的是“同时在执行多少个”；QPS 限制的是“请求进入得有多快”。

### 三层限制范围

| 范围 | 含义 |
| --- | --- |
| `global` | 整个进程的所有 `HttpConcurrencyLimiter` 实例共享。 |
| `service` | 当前 `Limiter` 实例。一个 `HttpLoadBalanceClient` 创建一个 `Limiter`，因此可以理解为当前这个下游服务的全部 `Endpoint` 合计限制。 |
| `endpoint` | 单个后端节点，通过 `endpoint_key` 分开计数。 |

`endpoint` 维度的计数示例：

```text
127.0.0.1:8081 → 3 个活跃请求
127.0.0.1:8082 → 1 个活跃请求
```

### 核心入口 `tryAcquire()`

整个准入过程是：

```text
申请全局并发
    ↓
申请服务并发 + Endpoint 并发
    ↓
申请全局 QPS 令牌
    ↓
申请服务 QPS 令牌
    ↓
申请 Endpoint QPS 令牌
    ↓
全部成功，返回 Guard
```

任何一步失败都返回 `nullptr`。

### 令牌桶如何限制 QPS

可以把它想象成一个装令牌的桶：

```text
请求到来
   ↓
桶里有令牌吗？
   ├── 有：拿走一个，请求通过
   └── 无：请求拒绝
```

令牌随时间补充：`补充数量 = 经过的毫秒数 × QPS / 1000`。

### 一句话理解

并发限制管“现在有多少请求正在执行”，QPS 限制管“请求进入得有多快”；任何一层超限都会立即拒绝请求。

## 4.5 熔断器

熔断器解决的问题是：某个后端持续超时或报错时，客户端不要继续把大量请求打过去；先快速拒绝，等待一段时间后再放少量探测请求，判断它是否恢复。

### 整体调用链

```text
选择 Endpoint
    ↓
Limiter 准入
    ↓
CircuitBreaker::tryAcquire()
    ├── 允许：发送 HTTP 请求
    │             ↓
    │    onRequestComplete(result)
    │             ↓
    │       更新熔断状态
    │
    └── 拒绝：返回 CIRCUIT_OPEN
              并尝试其他 Endpoint
```

### 状态切换过程

| 状态 | 含义 |
| --- | --- |
| `CLOSED` | 正常放行请求并记录结果。 |
| `OPEN` | 快速拒绝请求，等待冷却时间结束。 |
| `HALF_OPEN` | 放行少量探测请求，判断后端是否恢复。 |

```text
                   失败达到阈值
          ┌─────────────────────────┐
          │                         ↓
      CLOSED                      OPEN
          ↑                         │
          │                         │ 冷却时间结束
          │                         ↓
          └──── 探测成功 ───── HALF_OPEN
                                    │
                                    │ 探测失败
                                    └────→ OPEN
```

### 熔断器打开和恢复的完整例子

假设配置如下：

```cpp
consecutive_failure_threshold = 3;
open_timeout_ms = 5000;
half_open_max_requests = 1;
```

| 阶段 | 请求或事件 | 结果 |
| --- | --- | --- |
| 初始 | 无 | 状态为 `CLOSED`。 |
| 请求 1 | 连接失败 | 连续失败数变为 1，仍为 `CLOSED`。 |
| 请求 2 | 接收超时 | 连续失败数变为 2，仍为 `CLOSED`。 |
| 请求 3 | HTTP 503 | 连续失败数变为 3，切换为 `OPEN`。 |
| 接下来五秒 | `tryAcquire()` 返回 `nullptr` | 请求不发到后端。 |
| 五秒后 | 下一个请求到来 | 状态推进到 `HALF_OPEN`，允许一个探测请求。 |
| 探测成功 | `HALF_OPEN → CLOSED` | 失败记录清空。 |
| 探测失败 | `HALF_OPEN → OPEN` | 重新等待五秒。 |

### 一句话理解

熔断器在后端持续失败时先快速拒绝请求，冷却后再用少量探测请求决定是否恢复正常流量。

# 5. AI 模型调用网关（Mock）

## 5.1 如何被引入

AI 模型调用网关作为 Sylar 服务器的一个模块实现。

前面的 Module 章节已经介绍了模块加载流程：具体模块需要继承 `Module`、实现相应虚函数，
再由 `Application` 完成加载。通用框架本身并不知道 AI Gateway 需要哪些路由、Provider、
超时和治理参数，因此需要 `ai_gateway_module.cc` 连接框架与网关业务。

### 模块引入流程

```text
Sylar 通用框架（Application）
    ↓ 动态加载 libai_gateway_module.so
AiGatewayModule
    ↓ onServerReady()
读取 ai_gateway.yml
    ↓
找到名为 ai-gateway 的 HttpServer
    ↓
创建 Mock Client 或 RealProviderRuntime
    ↓
注册基本业务路由
    ↓
HTTP Server 开始接收请求
```

### 一句话理解

`Application` 负责加载通用模块，`AiGatewayModule` 负责把 AI Gateway 的配置、上游客户端和路由接入服务器。

## 5.2 路由与请求处理

`ai_gateway_module.cc` 负责在启动阶段注册路由；`AiGatewayServlet` 负责在路由命中后处理每一次请求。

| 组件 | 主要职责 |
| --- | --- |
| `ai_gateway_module.cc` | 读取配置、构造 upstream、注册业务路由。 |
| `ServletDispatch` | 根据请求路径找到对应的 Servlet。 |
| `AiGatewayServlet` | 解析请求、调用 upstream，并构造成功或错误响应。 |

### 请求处理流程

```text
HttpServer 收到请求
    ↓
ServletDispatch 根据路径查找 Servlet
    ↓
AiGatewayServlet::handle()
    ↓
解析 Chat Completions 请求
    ↓
调用 Mock 或真实 Provider
    ↓
转换成功响应或错误响应
    ↓
HttpServer 发送给客户端
```

### `upstreamPost` 的作用

`upstreamPost` 本身是一个函数回调，由 `ai_gateway_module.cc` 构造并注入
`AiGatewayServlet`。请求到达后，Sylar 先通过 `ServletDispatch` 找到对应 Servlet，
再由 `AiGatewayServlet::handle()` 调用这个回调访问外部接口。

| 处理内容 | 负责位置 |
| --- | --- |
| 请求校验 | `AiGatewayServlet` |
| 调用外部接口 | 注入的 `upstreamPost` 回调 |
| 构造成功响应 | `AiGatewayServlet` |
| 错误处理 | `AiGatewayServlet` |

## 5.3 Mock Provider 调用链

在 Mock 路径中，`ai_gateway_module.cc` 构造的 upstream 会请求本地 Mock Provider 的
`/v1/chat/completions` 接口。

### 两个同名路径

Mock 路径中出现了两个同名接口，但它们属于不同服务：

| 调用阶段 | 请求地址 | 路由归属 |
| --- | --- | --- |
| 客户端请求网关 | `POST http://127.0.0.1:18080/v1/chat/completions` | AI Gateway 路由 |
| 网关请求上游 | `POST http://127.0.0.1:19001/v1/chat/completions` | Mock Provider 路由 |

```text
客户端
    ↓ POST 127.0.0.1:18080/v1/chat/completions
AI Gateway
    ↓ 通过 HttpLoadBalanceClient 选择 Mock Provider
Mock Provider
    ↓ POST 127.0.0.1:19001/v1/chat/completions
返回模拟的模型响应
```

### Endpoint 与连接池

配置文件中保存 Mock Provider 的相关信息。程序根据配置创建 Endpoint，再为 Endpoint
创建连接池。发送请求时，先通过负载均衡选择 Endpoint，再从对应连接池中获取连接，
最后向 Mock Provider 发送请求。

```text
Mock Provider 配置
    ↓
创建 Endpoint
    ↓
为 Endpoint 创建连接池
    ↓
负载均衡选择 Endpoint
    ↓
从 Endpoint 的连接池获取连接
    ↓
向 Mock Provider 发送请求
```

### 一句话理解

客户端请求的是网关地址，网关再通过负载均衡和连接池请求 Mock Provider，因此两个同名路径对应不同端口和不同服务。

## 5.4 Real Provider 调用链

真实 Provider 路径还需要完成请求构造、上游调用和响应处理。

真实路径的 upstream 并不直接保存某一个真实 Provider 地址，而是一个调用
`RealProviderRuntime::execute()` 的函数：

```text
real_upstream
    = 调用 RealProviderRuntime::execute() 的函数
```

真正的 Provider 地址需要等请求到达并完成逻辑模型路由后才能确定。

### 一句话理解

Mock upstream 可以直接连接本地模拟服务；real upstream 则先进入 `RealProviderRuntime`，再根据逻辑模型路由决定真实上游。

## 5.5 解析协议

网关对外接收的是统一的粗粒度请求，等选定逻辑模型和真实 Provider 后，再组装成该
Provider 所需的完整 HTTP 请求。

| 阶段 | 网关处理 | 目的 |
| --- | --- | --- |
| 请求进入网关 | 解析统一的模型和消息字段 | 让客户端不依赖具体 Provider 的请求格式 |
| 选定 Provider | 补齐 URL、Header、JSON 和真实模型名 | 适配不同 Provider 的调用协议 |
| 收到上游响应 | 提取模型和响应内容 | 不直接暴露 Provider 的冗余字段 |
| 返回客户端 | 统一成功响应和错误结构 | 保持网关对外协议稳定 |

```text
统一网关请求
    ↓
选择逻辑模型对应的真实 Provider
    ↓
组装 Provider 专属 HTTP 请求
    ↓
解析 Provider 响应
    ↓
返回统一网关响应
```

这样可以屏蔽 Provider 差异、统一错误结构、支持切换 Provider，并隐藏具体供应商信息。

### 一句话理解

网关协议负责稳定对外格式，Provider 适配层负责把统一请求转换成不同上游真正需要的格式。

# 6. 真实 Provider 的实现

真实 Provider 路径用于完成项目闭环。相比基础 Mock Provider，真实调用还要处理模型映射、
尝试预算、限流、熔断、故障转移和协议适配。

## 6.1 完整调用链

```text
客户端请求
    ↓
AiGatewayServlet
解析 model / messages
    ↓
RealProviderRuntime
候选排序 + 创建共享预算
    ↓
ProviderAttemptExecutor       ← real_provider_gateway.h / .cc
路由、限流、熔断、尝试预算、故障转移
    ↓
OpenAICompatibleAdapter       ← ai_provider_adapter.h / .cc
构造 URL / Header / JSON，解析 Provider 响应
    ↓
HttpConnection::DoPost()
    ↓
真实模型 Provider
```

## 6.2 从 Module 调用真实 Provider

`RealProviderRuntime` 是真实 Provider 路径的运行时入口。Module 创建并初始化
`RealProviderRuntime`，收到请求后再通过它执行真实调用。

不同 Provider 的地址、路径、模型名和 API Key 环境变量可能都不相同；网关对前端则暴露
`general`、`image` 等自定义逻辑模型，再把逻辑模型映射到具有相应能力的真实 Provider。

由于真实调用可能产生费用，这条链路还要限制总尝试次数，避免故障转移时重复请求过多。

### 核心组件

以下组件主要定义在 `real_provider_gateway.h` 中：

| 组件 | 负责的问题 |
| --- | --- |
| `ProviderCandidate` | 保存一个真实 Provider 候选的地址、路径、模型和环境变量等参数 |
| `LogicalModelRouter` | 根据配置维护“逻辑模型 → 真实 Provider 候选”映射 |
| `RequestExecutionBudget` | 共享一次业务请求的尝试次数和时间预算，避免候选切换后重复放大请求 |
| `ProviderErrorCategory` / `ProviderErrorDecision` | 统一记录底层错误，并为故障转移和熔断决策提供依据 |
| `ProviderExecutionControls` | 组合 Provider 调用使用的限流器和熔断器 |
| `ProviderAttemptExecutor` | 整合路由、预算和治理组件，执行真实 Provider 候选调用 |

逻辑模型与真实模型的对应关系由管理者通过配置决定：功能相近的真实模型可以放进同一组
Provider 候选中。

### `RealProviderRuntime` 执行流程

```text
根据客户端选择的逻辑模型获取候选列表
    ↓
进入 ProviderAttemptExecutor::execute()
    ↓
检查共享预算并扣减一次尝试额度
    ↓
获取 concurrency limiter permit
    ↓
获取 circuit breaker permit
    ↓
调用 adapter handler
    ↓
分类调用结果并回报 circuit breaker
    ↓
直接返回结果，或继续尝试下一个候选
```

### 文件职责边界

| 文件 | 负责的问题 |
| --- | --- |
| `real_provider_gateway.*` | 模型路由、共享预算、错误决策、治理组件和候选执行 |
| `ai_provider_adapter.*` | 校验请求、拼装上游请求并解析 Provider 响应，同时避免泄露敏感信息 |
| `real_provider_runtime.*` | 读取配置，组装 router / controls / adapter，轮转候选并记录状态和 trace |
| `ai_gateway_servlet.*` | 校验客户端请求格式，并把真实调用结果写回 HTTP 响应 |

### 一句话理解

`RealProviderRuntime` 负责组装和驱动真实调用，`ProviderAttemptExecutor` 负责受预算与治理约束的
候选尝试，adapter 负责消除不同 Provider 的协议差异。

# 7. 请求发来以后项目是如何处理的

一个请求从客户端进入项目后，会依次经过 TCP 连接接收、HTTP 请求解析、Servlet 路由、
业务处理和 HTTP 响应发送。

## 6.1 完整处理链路

```text
客户端
  │ TCP 连接 / HTTP 字节流
  ▼
Socket::accept()
  ▼
TcpServer::startAccept()
  ▼
process_worker 调度连接处理协程
  ▼
HttpServer::handleClient()
  ▼
HttpSession::recvRequest()
  ▼
HttpRequestParser
  ▼
HttpRequest
  ▼
ServletDispatch 按 path 查找 Servlet
  ▼
AiGatewayServlet::handle()
  ▼
解析 JSON → 调用上游 Provider → 组装响应
  ▼
HttpSession::sendResponse()
  ▼
客户端收到 HTTP Response
```

## 6.2 先看路由部分

路由部分主要回答一个问题：HTTP 请求被解析以后，项目如何根据请求路径找到对应的
`Servlet`？

### 第一步：解析 HTTP 请求

例如客户端发送：

```http
POST /v1/chat/completions HTTP/1.1
Content-Type: application/json
Content-Length: ...

{"model":"demo","messages":[...]}
```

解析后会得到一个 `HttpRequest` 对象：

```text
method = POST
path   = /v1/chat/completions
headers
body   = JSON 字符串
```

### 第二步：HttpServer 把请求交给 ServletDispatch

`HttpServer` 处理请求时，会调用：

```cpp
m_dispatch->handle(req, rsp, session);
```

从这里开始，`ServletDispatch` 会判断具体由哪个 `Servlet` 处理当前请求。

### 第三步：注册路径与 Servlet 的映射

路由由 Server 添加，Server 内部保存一个 `ServletDispatch`。调用 `addServlet()` 时，
会把请求路径和对应的 `Servlet` 保存起来：

```cpp
void ServletDispatch::addServlet(const std::string &uri, Servlet::ptr slt)
{
    RWMutexType::WriteLock lock(m_mutex);
    m_datas[uri] = slt;
}
```

对应关系可以理解为：

```text
m_datas[
    "/v1/chat/completions"
] = shared_ptr<AiGatewayServlet>
```

### 第四步：根据 path 找到 Servlet

请求到达以后，`ServletDispatch` 根据 `HttpRequest` 中的 `path` 查找对应的
`Servlet`，再进入后续业务逻辑：

```text
HttpRequest.path = /v1/chat/completions
    ↓
ServletDispatch 查找 m_datas
    ↓
找到 AiGatewayServlet
    ↓
执行 AiGatewayServlet::handle()
```

### 一句话理解

Server 预先保存“请求路径 → Servlet”的映射，请求解析成 `HttpRequest` 后，
`ServletDispatch` 就能根据 `path` 找到对应的 `Servlet` 并继续处理。




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
