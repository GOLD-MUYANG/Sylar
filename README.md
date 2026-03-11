# Sylar 服务框架

## 介绍

现在是学习的 Sylar 项目，本项目实现的一个 Linux 下的高性能服务器框架，包括日志管理等功能。目前已完成最最最基本的日志系统。

<br>

## 1、日志系统

### 日志系统整体流程（简版）

1. **创建 Logger**

   * Logger 会带着一套默认格式字符串。
   * 这套格式被交给 `Formatter`，`Formatter` 会解析并生成一组 `FormatItem`。
   * 之后实际的字符串拼接，就是这些 `FormatItem` 完成的。

2. **添加 Appender（输出端）**

   * 可以加控制台输出、文件输出，或同时加。
   * 每个 Appender 默认使用 Logger 自己的 Formatter。
   * 也可以在创建 Appender 时传入新的 Formatter，让不同的 Appender 输出不同风格。

3. **日志真正打印的过程**

   * 日志宏生成 `LogEvent`，由 `LogEventWrap` 在析构时触发打印。
   * 打印会调用 `logger.log()`。
   * Logger 依次调用所有 Appender 的 `log()`。
   * Appender 使用自己的 `Formatter.format()`，由其中的 `FormatItem` 把日志内容拼好。
   * 拼好的字符串写入控制台或文件。

这就是整个系统的核心链路：  
**Logger → Appender → Formatter → FormatItem拼接字符串 → StringStream输出**

<br><br><br>

## 2、配置系统与日志系统整合流程

### 1. 初始化日志配置项

1. 定义 `LogDefine` 类，作为日志配置（日志器、格式化器、输出器）的载体；
2. 向配置系统注册 `logs` 配置项，默认值为空 `LogDefine` 实例；
3. 为 `logs` 配置项绑定回调函数，用于配置变更时更新日志系统。

### 2. 加载并解析配置文件

1. 配置系统加载配置文件，将层级化配置 Key 展平，保留对应节点；
2. 遍历展平后的配置项，匹配到 `logs` 配置项时，将其节点序列化为字符串；
3. 将字符串反序列化为新的 `LogDefine` 实例，更新配置系统中 `logs` 对应的值。

### 3. 触发回调初始化日志系统

1. `logs` 配置项的值更新后，自动触发绑定的回调函数；
2. 回调函数解析新的 `LogDefine` 实例，生成/更新日志器、格式化器、输出器；
3. 将生成的日志组件整合，完成日志系统的初始化并生效。

## 3、线程库的封装，为日志系统和配置系统添加锁的处理

1. 这一部分没什么好说的，主要是用 Linux 提供的系统方法去封装了锁和多线程，在日志系统读取和修改 logger 或 appender 之前加上锁；在配置系统读取和写配置之前加上锁。

## 4、协程

1. 封装 ucontext 库，实现协程的上下文切换。
2. 执行传入的需要协程来执行的方法。

## 5、协程调度器模块

1. 使用方法：

    1.1  
    创建 Scheduler 对象，调用 `schedule()` 放入需要执行的任务，开启调度器 `start()`。调度器将自动开启线程去处理任务。最后关闭调度器 `stop()`。

2. 原理：

    2.1  
    n 对 m 的协程调度器，n 个线程，执行 m 个任务。每个线程会从任务队列里取出任务执行，当任务队列为空时，线程会阻塞等待新的任务到来。

    2.2  
    难点在于如何让包含执行 main 函数的线程，也去执行任务队列里的任务。  
    `m_rootFiber.reset(new Fiber(std::bind(&Scheduler::run, this), 0, true));`  
    关键是理解 scheduler 里的这一行代码：这里新建了一个协程去执行 `run()`。

## 6、IO调度、Timer、Hook模块

use_caller为ture的情况下：

1. 创建iomanager，初始化scheduler(对当前的线程，协程进行初始化，并绑定run方法)，准备epoll，开启调度（satrt()）
start (开线程，绑定run)

2. 加入任务（schedule）

3. iomanager析构，执行schduler的stop方法，Fiber如果判断还不能停止（有任务，有定时器，有活跃线程）,就执行Fiber::call，因为只有m_rootFiber

4. run（）<br>
    拿到当前线程可以执行的任务

    拿到以后，当前线程更改上下文去执行任务

    正常执行的，执行完返回了；阻塞的，挂起了，等待idle线程将执行完的任务将阻塞任务设置为reday后重新调度进去

    触发回调，进入hook sleep
5. sleep():<br>
    当作任务加入到定时器中（addTimer）

    当前任务在定时器任务的最前面，就得让idle线程来处理一下(不过得等idle线程起来（也就是任务都加完结果都是阻塞的）)

    （对于前面有个耗时长的任务，idle起不来，定时器任务没法执行了）

    sleep等待执行中，返回到run，继续加任务
    需要立马执行的任务处理完毕，但是还没有满足停止的条件，那么进入IOManger::idle，
6. idle():<br>
    找过期的定时器任务，重新调度（schedule()）,

    看被阻塞的任务是否可以运行了（来了读写事件），也重新调度

    看其他socket事件，没有需要处理的，返回run

    run中，定时器任务已经调度进去了，现在定时器任务开始执行。
    都执行完了，
    
    
    （注意idle是一个while，一直在调用，每次看完有没有新任务，有没有定时器到期，只是swapout，
        只有满足stopping（）条件，才会真正break掉，然后MainFunc才能走下一步，把idle的状态改成TERM，才能开始退出
    ）
   
7. idle又进去了，没任务，idle swapout，再次运行，没任务，判断idle方法返回，run返回

8. 退出