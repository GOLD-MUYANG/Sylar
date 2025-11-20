
# Sylar 服务框架

## 介绍

现在是学习的 Sylar 项目，本项目实现的一个 Linux 下的高性能服务器框架，包括日志管理等功能。目前已完成最最最基本的日志系统。

<br>

>## 1、日志系统
>  
> ### 1.1 类结构与执行流程
> 
> 
> 
> 1. **Logger** 持有多个 **Appender**（输出目的地）
> <br>
> 2. 每个 **Appender** 持有自己的 **Formatter**
> <br>
> 3. **Formatter** 在构造时立即解析 pattern，并生成一组 **FormatItem** 子对象
> <br>
> 4. 当真正需要输出日志时，**Appender** 调用 `Formatter`
> <br>
> 5. **Formatter** 遍历内部的 `FormatItem (m_items)`，依次调用每个 `FormatItem`
> <br>
> 6. 所有 FormatItem 把 **LogEvent** 的信息拼成完整字符串，最终输出到控制台或文件
> <br>

<br><br><br>
