# 2026-05-28 修复 HttpConnectionPool 连接池

## 背景

根据 `文档/接下来要做什么.md` 中“三、推荐建设顺序”的第一步，修正
`HttpConnectionPool` 的连接生命周期、最大连接数、等待/超时，并按要求使用
`www.baidu.com` 做连接池各项测试。

## 排查过程

1. `sed -n '/^三、推荐建设顺序/,$p' 文档/接下来要做什么.md`
   - 原因：确认第一步的明确要求，避免扩大范围。

2. `rg -n "HttpConnectionPool|m_createTime|max_alive|maxSize|max_size|m_maxSize" sylar tests CMakeLists.txt`
   - 原因：定位连接池实现、字段和已有测试入口。

3. `sed -n '1,360p' sylar/http/http_connection.cc`
   - 原因：阅读 `getConnection()`、`ReleasePtr()` 和请求发送路径，确认根因。

4. `sed -n '1,260p' sylar/http/http_connection.h`
   - 原因：确认连接池公开接口和内部状态，决定以内部等待队列实现容量限制。

5. `sed -n '1,280p' sylar/mutex.h`、`sed -n '1,260p' sylar/mutex.cc`
   - 原因：查看已有锁、协程信号量和调度相关原语，避免使用会阻塞调度线程的等待方式。

6. `sed -n '1,260p' sylar/scheduler.h`、`sed -n '1,220p' sylar/fiber.h`、`sed -n '1,260p' sylar/iomanager.h`
   - 原因：确认等待队列可用 `Scheduler + Fiber + Timer` 实现协程等待和超时唤醒。

7. `cmake --build build --target test_http_connection`
   - 原因：构建基于 `www.baidu.com` 的连接池回归测试。

8. `./bin/test_http_connection`
   - 原因：运行新增测试，验证连接复用、最大请求次数、最大存活时间、池满等待和池满超时。

9. `gdb -batch -ex run -ex bt --args ./bin/test_http_connection`
   - 原因：初版测试出现段错误，用栈信息确认是测试断言后继续解引用空响应，随后修正测试保护逻辑。

10. `cmake --build build`
    - 原因：源码修正后做完整构建验证。

11. `./bin/test_http_connection`
    - 原因：验证连接池回归测试通过。该测试依赖访问 `www.baidu.com`。

## 根因

- 新建连接没有设置 `m_createTime`。
- `max_alive_time` 判断方向反了，导致未过期连接被当成无效、过期连接反而可能被复用。
- `m_maxSize` 只保存配置，没有限制总连接数。
- 池满时没有等待队列，也没有等待超时。
- 归还连接时没有统一判断连接是否仍可复用。
- HTTP 响应解析没有把 `Connection: close` 状态同步回 `HttpResponse`，影响连接归还时的复用判断。

## 验证结果

- `cmake --build build`：通过。
- `./bin/test_http_connection`：通过。该测试依赖访问 `www.baidu.com`。
