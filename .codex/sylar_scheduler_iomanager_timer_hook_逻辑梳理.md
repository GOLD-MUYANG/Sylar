# Sylar 调度核心逻辑梳理（Scheduler / IOManager / Timer / Hook）

本文对应源码：
- `sylar/scheduler.h` `sylar/scheduler.cc`
- `sylar/iomanager.h` `sylar/iomanager.cc`
- `sylar/timer.h` `sylar/timer.cc`
- `sylar/hook.h` `sylar/hook.cc`

## 1. 总体分层关系

核心继承关系：

`IOManager : public Scheduler, public TimerManager`

含义是：
- `Scheduler` 负责“执行队列里的任务（Fiber/回调）”
- `TimerManager` 负责“管理定时器并产出到期回调”
- `IOManager` 把两者接起来，再加上 `epoll` 处理 IO 就绪事件

一句话概括：
1. 任务进队列（`schedule`）
2. 线程在 `Scheduler::run()` 里取任务执行
3. 没任务时进 `idle()`；在 `IOManager` 里就是 `epoll_wait`
4. IO 就绪或定时器到期后，把回调/Fiber 再次 `schedule` 回调度器

---

## 2. Scheduler（协程调度器）做了什么

### 2.1 关键数据结构
- `m_fibers`：任务队列，元素是 `FiberAndThread`（任务 + 指定线程）
- `m_threads`：工作线程池
- `m_rootFiber`：当 `use_caller=true` 时，调用者线程上的调度协程
- `m_activeThreadCount / m_idleThreadCount`：活跃线程和空闲线程计数

`FiberAndThread` 支持两类任务：
- 直接给 `Fiber::ptr`
- 给 `std::function<void()>`，运行时再包成 Fiber

### 2.2 生命周期
- 构造（`Scheduler::Scheduler`）  
  如果 `use_caller=true`，当前线程也参与调度，创建 `m_rootFiber(run)`
- 启动（`Scheduler::start`）  
  创建 `m_threadCount` 个线程，每个线程入口都是 `Scheduler::run`
- 停止（`Scheduler::stop`）  
  设停止标记 + `tickle()` 唤醒阻塞线程，最后 `join` 线程

### 2.3 run 主循环（最关键）
`Scheduler::run` 的循环逻辑：
1. 从 `m_fibers` 取一个“当前线程可执行”的任务
2. 执行任务（`fiber->swapIn()` 或 `cb_fiber->swapIn()`）
3. 执行后根据 Fiber 状态决定：
   - `READY`：重新入队
   - `TERM/EXCEPT`：结束
   - 其他：置 `HOLD`
4. 若无任务，执行 `idle_fiber->swapIn()`（进入 `idle()`）

---

## 3. IOManager 如何把 epoll 接入调度器

### 3.1 FdContext：fd 与事件上下文绑定
每个 fd 有一个 `FdContext`，内部有：
- `read` 事件上下文
- `write` 事件上下文
- 当前注册事件位 `events`

每个事件上下文 `EventContext` 保存：
- `scheduler`
- `fiber` 或 `cb`

触发时（`triggerEvent`）会：
1. 从 `events` 清掉该事件位
2. 把 `cb/fiber` `schedule` 回对应调度器

### 3.2 addEvent / delEvent / cancelEvent
- `addEvent`：
  - 给 fd 注册 `READ/WRITE` 到 epoll
  - 同时保存触发后要执行的 `fiber/cb`
  - `m_pendingEventCount++`
- `delEvent`：仅移除监听，不触发回调
- `cancelEvent`：移除监听并“强制触发一次”对应回调/Fiber

### 3.3 IOManager::idle（本质是 epoll 事件循环）
`IOManager::idle()` 里做三件事：
1. 用 `getNextTimer()` 计算 epoll 超时时间
2. `epoll_wait(...)` 等待 IO 或 tickle 管道事件
3. 醒来后：
   - 先取到期定时器回调 `listExpiredCb` 并 `schedule`
   - 再处理每个 epoll 事件，触发 `FdContext::triggerEvent`
   - 最后当前 idle 协程 `swapOut`，回到 `Scheduler::run` 继续调度

---

## 4. TimerManager 如何驱动“超时唤醒”

### 4.1 存储模型
- `m_timers`：`std::set<Timer::ptr, Comparator>`
- 排序键是 `m_next`（下次触发时间）

### 4.2 关键接口
- `addTimer`：插入定时器；如果插到队头，调用 `onTimerInsertedAtFront()`
- `getNextTimer`：返回离现在最近的超时时间（给 epoll timeout）
- `listExpiredCb`：取出所有到期定时器回调
  - 周期定时器：重算 `m_next` 后重新插入
  - 非周期：置空回调

在 `IOManager` 里，`onTimerInsertedAtFront()` 会 `tickle()`，让阻塞的 `epoll_wait` 及时重算超时。

---

## 5. Hook 如何把“阻塞调用”变成“协程让出”

### 5.1 开关
- 线程局部开关：`t_hook_enable`
- 在 `Scheduler::run` 开始处调用 `set_hook_enable(true)`，所以调度线程内默认开启 hook

### 5.2 sleep/usleep/nanosleep 的改造
逻辑是：
1. 不开 hook：直接系统调用
2. 开 hook：创建定时器，到期后 `schedule(当前fiber)`
3. 当前 fiber `YieldToHold()` 让出
4. 到期后被重新调度，继续执行

### 5.3 do_io 模板（核心思想）
`do_io(...)` 的思路是：
1. 若不满足 hook 条件（未开启、非 socket、用户显式非阻塞等），直接系统调用
2. 先试一次系统调用
3. 若返回 `EAGAIN`：
   - 向 `IOManager` 注册对应 `READ/WRITE` 事件
   - 可选加一个超时定时器，超时后 `cancelEvent`
   - 当前 Fiber `YieldToHold()`
4. 被 IO 事件或超时唤醒后重试调用

这就是“看起来阻塞、实际让出协程”的关键机制。

---

## 6. 一条完整链路（把四块拼起来）

以“协程里 read 读不到数据”为例：

1. 业务协程调用（未来会 hook 的）`read`
2. `do_io` 首次调用真实 `read` 返回 `EAGAIN`
3. `IOManager::addEvent(fd, READ)`，并记录“哪个 Fiber 等这个事件”
4. 当前协程 `YieldToHold()`，线程去跑别的任务
5. 线程空闲时进 `IOManager::idle()`，阻塞在 `epoll_wait`
6. fd 可读后，`epoll_wait` 返回，`triggerEvent(READ)` 把等待的 Fiber 重新 `schedule`
7. `Scheduler::run()` 再次执行这个 Fiber，回到 `do_io` 的 retry 重试 `read`
8. 成功读到数据，业务协程继续向下执行

---

## 7. 停止条件如何判定

`IOManager::stopping(timeout)` 需要同时满足：
- 没有定时器（`getNextTimer() == ~0ull`）
- 没有挂起 IO 事件（`m_pendingEventCount == 0`）
- `Scheduler::stopping()` 为真（自动停止标记 + 无待调度任务 + 无活跃线程）

只有这三类工作都清空，线程才会退出。

---

## 8. 当前代码状态下需要特别注意的点

1. `hook.cc` 在当前仓库中是不完整状态  
   目前只看到 `sleep/usleep/nanosleep` 和 `do_io` 模板，文件在大量 `typedef` 处结束；`hook.h` 声明的许多函数（如 `connect/read/write/close/fcntl...`）在该文件未实现。  
   所以“完整 hook 链路”是设计上可见，但当前代码并未全部落地。

2. `do_io` 里 `while (n == -1 && EINTR)` 这一行条件可疑  
   语义上通常应判断 `errno == EINTR`。当前写法会导致只要 `n==-1` 就持续重试（因为 `EINTR` 常量非 0）。

3. `IOManager::tickle()` 里 `if (hasIdleThreads()) return;` 语义可疑  
   常见设计是“有空闲线程时才需要唤醒”。这里是反向判断，可能是本项目特意策略，也可能是逻辑问题，建议结合实际运行再核对。

---

## 9. 建议阅读顺序（最快建立脑图）

1. `Scheduler::run`（先看“任务怎么跑”）
2. `IOManager::idle`（再看“没任务时线程在干嘛”）
3. `IOManager::addEvent + FdContext::triggerEvent`（看“IO 事件如何回到任务队列”）
4. `TimerManager::getNextTimer + listExpiredCb`（看“定时器如何插入同一套调度”）
5. `hook.cc` 的 `sleep/usleep/nanosleep + do_io`（看“业务阻塞调用如何变成协程让出”）

这样读下来，你会把它理解成一个统一模型：
- Scheduler 是执行器
- IO 和 Timer 只是“生产可执行任务”的两种来源
- Hook 只是“把同步阻塞 API 转成 IO/Timer 事件并挂回 Scheduler”

---

## 10. `tests/test_hook.cc` 实际运行调用链（只覆盖 `sleep`）

### 10.1 调用链记录

以 `tests/test_hook.cc` 的实际运行过程为准，只保留项目自定义函数之间的进入与返回。  
其中两个任务协程流程相同，仅 `sleep` 时长不同，下面分别记为“任务协程1 / 任务协程2”。

```text
1. main
  -> test_sleep
    -> IOManager::IOManager
      -> Scheduler::Scheduler
        -> Fiber::GetThis
        <- Fiber::GetThis 返回 Scheduler::Scheduler
      <- Scheduler::Scheduler 返回 IOManager::IOManager

      -> IOManager::contextResize
      <- IOManager::contextResize 返回 IOManager::IOManager

      -> Scheduler::start
      <- Scheduler::start 返回 IOManager::IOManager
    <- IOManager::IOManager 返回 test_sleep

    -> Scheduler::schedule            [提交任务协程1]
    <- Scheduler::schedule 返回 test_sleep

    -> Scheduler::schedule            [提交任务协程2]
    <- Scheduler::schedule 返回 test_sleep
  <- test_sleep 返回 main
```

```text
2. main 结束 test_sleep 作用域，开始析构 IOManager
  -> IOManager::~IOManager
    -> Scheduler::stop
      -> Fiber::call                  [切到 m_rootFiber]
        -> Scheduler::run

          -> Fiber::GetThis           [初始化当前线程主协程]
          <- Fiber::GetThis 返回 Scheduler::run

          -> Fiber::swapIn            [执行任务协程1]
            -> Fiber::MainFunc
              -> 测试回调(任务协程1)
                -> TimerManager::addTimer
                  -> TimerManager::addTimer(val, lock)
                    -> IOManager::onTimerInsertedAtFront
                      -> IOManager::tickle
                      <- IOManager::tickle 返回 IOManager::onTimerInsertedAtFront
                    <- IOManager::onTimerInsertedAtFront 返回 TimerManager::addTimer(val, lock)
                  <- TimerManager::addTimer(val, lock) 返回 TimerManager::addTimer
                <- TimerManager::addTimer 返回 测试回调(任务协程1)

                -> Fiber::YieldToHold
                  -> Fiber::GetThis
                  <- Fiber::GetThis 返回 Fiber::YieldToHold
                  -> Fiber::swapOut
                  <- Fiber::swapOut 返回 Fiber::YieldToHold
                <- Fiber::YieldToHold 返回 测试回调(任务协程1暂停点)
              [任务协程1挂起，未返回]
            [切回 Scheduler::run]
          <- Fiber::swapIn 返回 Scheduler::run

          -> Fiber::swapIn            [执行任务协程2]
            -> Fiber::MainFunc
              -> 测试回调(任务协程2)
                -> TimerManager::addTimer
                  -> TimerManager::addTimer(val, lock)
                  <- TimerManager::addTimer(val, lock) 返回 TimerManager::addTimer
                <- TimerManager::addTimer 返回 测试回调(任务协程2)

                -> Fiber::YieldToHold
                  -> Fiber::GetThis
                  <- Fiber::GetThis 返回 Fiber::YieldToHold
                  -> Fiber::swapOut
                  <- Fiber::swapOut 返回 Fiber::YieldToHold
                <- Fiber::YieldToHold 返回 测试回调(任务协程2暂停点)
              [任务协程2挂起，未返回]
            [切回 Scheduler::run]
          <- Fiber::swapIn 返回 Scheduler::run

          -> Fiber::swapIn            [执行 idle 协程]
            -> Fiber::MainFunc
              -> IOManager::idle

                -> IOManager::stopping
                  -> IOManager::stopping(timeout)
                    -> TimerManager::getNextTimer
                    <- TimerManager::getNextTimer 返回 IOManager::stopping(timeout)
                  <- IOManager::stopping(timeout) 返回 IOManager::stopping
                <- IOManager::stopping 返回 IOManager::idle

                -> TimerManager::listExpiredCb      [2 秒到期，取出任务协程1对应回调]
                <- TimerManager::listExpiredCb 返回 IOManager::idle

                -> Scheduler::schedule              [重新调度任务协程1]
                <- Scheduler::schedule 返回 IOManager::idle

                -> Fiber::GetThis
                <- Fiber::GetThis 返回 IOManager::idle

                -> Fiber::swapOut
                <- Fiber::swapOut 返回 IOManager::idle
              [idle 协程让出]
            [切回 Scheduler::run]
          <- Fiber::swapIn 返回 Scheduler::run

          -> Fiber::swapIn            [恢复任务协程1]
            -> 测试回调(任务协程1) 从挂起点继续
            <- 测试回调(任务协程1) 返回 Fiber::MainFunc
            -> Fiber::swapOut
            <- Fiber::swapOut 返回 Fiber::MainFunc
          <- Fiber::swapIn 返回 Scheduler::run

          -> Fiber::swapIn            [再次执行 idle 协程]
            -> IOManager::idle

              -> IOManager::stopping
                -> IOManager::stopping(timeout)
                  -> TimerManager::getNextTimer
                  <- TimerManager::getNextTimer 返回 IOManager::stopping(timeout)
                <- IOManager::stopping(timeout) 返回 IOManager::stopping
              <- IOManager::stopping 返回 IOManager::idle

              -> TimerManager::listExpiredCb        [3 秒到期，取出任务协程2对应回调]
              <- TimerManager::listExpiredCb 返回 IOManager::idle

              -> Scheduler::schedule                [重新调度任务协程2]
              <- Scheduler::schedule 返回 IOManager::idle

              -> Fiber::GetThis
              <- Fiber::GetThis 返回 IOManager::idle

              -> Fiber::swapOut
              <- Fiber::swapOut 返回 IOManager::idle
            [idle 协程让出]
          <- Fiber::swapIn 返回 Scheduler::run

          -> Fiber::swapIn            [恢复任务协程2]
            -> 测试回调(任务协程2) 从挂起点继续
            <- 测试回调(任务协程2) 返回 Fiber::MainFunc
            -> Fiber::swapOut
            <- Fiber::swapOut 返回 Fiber::MainFunc
          <- Fiber::swapIn 返回 Scheduler::run

          -> Fiber::swapIn            [最后一次执行 idle 协程]
            -> IOManager::idle
              -> IOManager::stopping
                -> IOManager::stopping(timeout)
                  -> TimerManager::getNextTimer
                  <- TimerManager::getNextTimer 返回 IOManager::stopping(timeout)
                  -> Scheduler::stopping
                  <- Scheduler::stopping 返回 IOManager::stopping(timeout)
                <- IOManager::stopping(timeout) 返回 IOManager::stopping
              <- IOManager::stopping 返回 IOManager::idle
            <- IOManager::idle 返回 Fiber::MainFunc
            -> Fiber::swapOut
            <- Fiber::swapOut 返回 Fiber::MainFunc
          <- Fiber::swapIn 返回 Scheduler::run

        <- Scheduler::run 返回 Fiber::call
      <- Fiber::call 返回 Scheduler::stop
    <- Scheduler::stop 返回 IOManager::~IOManager
  <- IOManager::~IOManager 返回 main
```

### 10.2 关键转折点

1. `Scheduler::run` 开头调用 `set_hook_enable(true)`，所以测试回调里的 `sleep(...)` 进入的是项目自己的 hook 逻辑，不再直接阻塞线程。

2. `sleep` 的核心动作不是等待，而是：
   `TimerManager::addTimer -> 到期后 schedule(当前 fiber) -> Fiber::YieldToHold`
   当前任务协程先挂起，让调度器去跑别的协程。

3. 两个任务协程都挂起后，`Scheduler::run` 没有普通任务可跑，于是转入 `IOManager::idle`。

4. `IOManager::idle` 每次被唤醒，先通过 `TimerManager::listExpiredCb` 取出到期定时器回调，再通过 `Scheduler::schedule` 把原先挂起的任务协程重新放回调度队列。

5. 任务协程被重新 `swapIn` 后，不会从头执行，而是从 `Fiber::YieldToHold` 之后继续往下跑，所以你看到的是 `sleep(2)` 对应任务先恢复，`sleep(3)` 对应任务后恢复。

6. 最后一轮 `IOManager::idle` 中，`IOManager::stopping(timeout)` 与 `Scheduler::stopping()` 同时满足，调度循环结束，`Scheduler::stop` 返回，随后 `IOManager` 析构完成。
