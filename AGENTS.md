# Repository Guidelines

## 项目结构
- `sylar/`：框架核心源码。调度、协程、IO、Hook、Socket、配置、日志、Application、Module 都在这里。
- `sylar/http/`：HTTP、WebSocket、Servlet、HTTP Session/Connection/Server 相关实现。
- `sylar/util/`：工具类实现，例如 hash 工具。
- `tests/`：测试程序源码，命名多为 `test_<module>.cc`。
- `examples/`：示例程序，目前有 `echo_server.cc`。
- `bin/conf/`：运行配置，包含 `server.yml`、`worker.yml`、`log.yml`。
- `cmake/`：CMake 辅助函数，包含 Ragel 生成规则和可执行目标封装。

## 代码结构
  - 日志系统：sylar/log.cc。
  - 配置系统：sylar/config.cc 读取 YAML 配置目录并加载 .yml。
  - 命令行/环境：sylar/env.cc 解析 -s、-d、-c 等参数。
  - 守护进程：sylar/daemon.cc 根据参数决定前台或 daemon 运行。
  - 调度器/协程：sylar/scheduler.cc 管理任务队列、线程和协程调度。
  - IO 调度：sylar/iomanager.cc 使用 epoll 和管道唤醒机制。
    iomanager.cc:354
  - Hook：sylar/hook.cc 用 dlsym(RTLD_NEXT, ...) 替换系统 IO 调用，并接入 IOManager。
  - TCP Server：sylar/tcp_server.cc 负责 bind/listen/accept，并把连接交给 worker。
  - HTTP Server：sylar/http/http_server.cc 接收请求、派发 Servlet、发送响应。
  - 模块系统：sylar/module.cc 扫描 .so，sylar/library.cc 通过 CreateModule/DestoryModule 加载模块。

## 构建命令
- `cmake -S . -B build`：生成构建目录。
- `cmake --build build`：编译 `lib/libsylar.so`、`bin/sylar`、当前启用的测试和模块库。
- 构建依赖：`yaml-cpp`、`pthread`、`dl`、OpenSSL、Ragel。
- CMake 当前默认是 Release：`-O3 -DNDEBUG -std=c++11`。

## 运行与测试命令
- `./bin/sylar -s -c bin/conf`：前台运行主服务。
- `./bin/sylar -d -c bin/conf`：守护进程方式运行主服务。
- `./bin/test_http_connection`：运行当前 CMake 启用的测试目标。
- 其他 `tests/test_*.cc` 多数已在 `CMakeLists.txt` 中注释，不能假设会自动编译；需要测试某个模块时，先确认或启用对应 `sylar_add_executable(...)`。

## 编码约束
- 遵循 `.clang-format`：LLVM 基础风格，4 空格缩进，Allman 大括号，100 列限制。
- 框架代码放在 `sylar` 命名空间；HTTP 相关代码放在 `sylar::http`。
- 源码文件使用小写加下划线，例如 `tcp_server.cc`、`http_connection.h`。
- 成员变量以 `m_` 开头，全局变量以 `g_` 开头，线程局部变量以 `t_` 开头。
- 类内顺序：`typedef`/类型定义在前；再按 `public`、`protected`、`private` 排列；每个访问区内先内部类，再方法，再成员变量。头文件内直接实现的方法放在同一区域靠前位置。
- 新测试放在 `tests/`，命名为 `test_<module>.cc`，并通过 `sylar_add_executable(...)` 接入构建。

## 验证流程
1. 文档或配置变更：至少检查相关文件内容和路径引用是否准确。
2. C++ 源码变更：运行 `cmake --build build`。
3. 涉及 HTTP Connection：运行 `./bin/test_http_connection`，并说明它依赖外网。
4. 涉及服务启动、Application、Worker、配置加载：运行 `./bin/sylar -s -c bin/conf`，观察日志和 pid 文件行为。
5. 涉及新增测试目标：确认 `CMakeLists.txt` 中目标已启用，再构建并运行对应 `bin/test_*`。

## 常见坑
- 主程序必须带 `-s` 或 `-d`，否则 `Application::init()` 会打印帮助并退出。
- `test_http_connection` 访问 `www.baidu.com` 和 `www.sylar.top`，网络不可用时可能失败。
- `sylar/http/http11_parser.rl.cc`、`sylar/http/httpclient_parser.rl.cc`、`sylar/uri.rl.cc` 是 Ragel 生成文件；优先修改对应 `.rl`。
- `build/`、`lib/`、`bin/sylar`、`bin/test_*`、`bin/module/*.so` 是构建产物，不应手工编辑。
- `bin/conf/log.yml` 默认写 `/apps/logs/sylar/*.txt`，本地没有目录时日志文件输出可能失败。
- `tmp/*.pid`、`bin/work/*.pid` 是运行状态文件，不应作为源码提交依据。

## Agent 规则
- 所有输出使用中文。

## Unknowns
- README 的开头状态描述可能滞后于当前源码；以源码和 CMake 为准。
- `generate.sh` 不应继续使用；最近提交信息显示已放弃它。
