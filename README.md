
# Sylar 服务框架

## 介绍

现在是学习的 Sylar 项目，本项目实现的一个 Linux 下的高性能服务器框架，包括日志管理等功能。目前已完成最最最基本的日志系统。

<br>

##  1、日志系统
 <!-- 创建一个logger，首先类实例化会传入一串默认的字符串进行格式化，传入Fomatter，Formatter根据这串字符串去init，也就是说会生成对应的FormatItem，后面实际的输出字符串的拼接，其实就是这些FormatItem进行的。然后可选输出到控制台或者文件（Appender），再或者两者都要。Appender默认是使用的Logger里面的Formatter，如果在Appender初始化的时候传入一个新的Formatter，并指定输出的格式，就会用新的输出格式去输出。实际的输出，其实还是从Appender那里拿到的，所以，不同的Appender可以指定出不同的输出样式。同一个logger可以拥有多个Appender，也就保证了Logger有不同的输出样式。

 实际的打印，通过LogEventWrap析构，会拿到event里的logger，然后logger.log输出，拿到logger里的所有appender，用appender.log输出，去调用appender里的formatter.format，去调用fomatter解析出来的FormatItem进行需要打印的字符串的拼接，由stringStream去实际的接收要打印的字符串。 -->


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

<br ><br><br>
 
<!-- 
先把会存logdefine的类写好，去查map，发现没有，设置默认值（其实就是什么也没有，只有一个类），然后给他添加一个回调函数
开始加载配置文件，会把配置文件key展平，value还是node，遍历每一个配置项，然后把node转成string放到对应的configVar里进行管理，
为什么能知道我要转的就是LogDefine呢，是因为我map里面只有一个logs，默认值就是LogDefine，读配置文件，读到logs，发现是要修改，
那么就是按照logdefine去修改，修改的时候就会触发回调函数，把配置文件里的logger，Formatter，appender全都整合生成具体的类去实现日志系统 -->

## 配置系统与日志系统整合流程
### 1. 初始化日志配置项
1. 定义`LogDefine`类，作为日志配置（日志器、格式化器、输出器）的载体；
2. 向配置系统注册`logs`配置项，默认值为空`LogDefine`实例；
3. 为`logs`配置项绑定回调函数，用于配置变更时更新日志系统。

### 2. 加载并解析配置文件
1. 配置系统加载配置文件，将层级化配置Key展平，保留对应节点；
2. 遍历展平后的配置项，匹配到`logs`配置项时，将其节点序列化为字符串；
3. 将字符串反序列化为新的`LogDefine`实例，更新配置系统中`logs`对应的值。

### 3. 触发回调初始化日志系统
1. `logs`配置项的值更新后，自动触发绑定的回调函数；
2. 回调函数解析新的`LogDefine`实例，生成/更新日志器、格式化器、输出器；
3. 将生成的日志组件整合，完成日志系统的初始化并生效。

## 线程库的封装，为日志系统和配置系统添加锁的处理
1、这一部分没什么好说的，主要是用linux提供的系统方法去封装了锁和多线程，在日志系统读取和修改logger或appender之前加上锁；在配置系统读取和写配置的之前加上锁。

## 协程
1、封装ucontext库，实现协程的上下文切换
2、执行传入的需要协程来执行的方法

### 4、协程调度器模块
1、使用方法：

    1.1、 创建Scheduler对象，调用schedule()放入需要执行的任务，开启调度器（start()）。调度器将自动开启线程去进行任务的处理。最后关闭调度器（stop()）;
2、原理：

    2.1、n对m的协程调度器，n个线程，执行m个任务，每个线程会从任务队列里取出任务执行，当任务队列为空时，线程会阻塞等待新的任务到来。

    2.2、难点在于如何用包含执行main函数的进程也去执行任务队列里的任务。
    m_rootFiber.reset(new Fiber(std::bind(&Scheduler::run, this), 0, true));关键是理解scheduler里的这一行代码，是新建了一个协程去执行run。
### IO调度模块
    流程暂不分析。后面可能单独走一遍
    大概就是监听文件的读写事件（epoll），然后用scheduler去进行调度事件。
