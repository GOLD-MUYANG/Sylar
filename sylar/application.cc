#include "application.h"
#include "sylar/config.h"
#include "sylar/daemon.h"
#include "sylar/env.h"
#include "sylar/log.h"
#include <unistd.h>

namespace sylar
{

// 全局日志对象，名字为 "system"
static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

// 配置项：服务器工作路径，默认 "/apps/work/sylar"
static sylar::ConfigVar<std::string>::ptr g_server_work_path =
    sylar::Config::Lookup("server.work_path", std::string("/apps/work/sylar"), "server work path");

// 配置项：PID 文件名，默认 "sylar.pid"
static sylar::ConfigVar<std::string>::ptr g_server_pid_file =
    sylar::Config::Lookup("server.pid_file", std::string("sylar.pid"), "server pid file");

// HTTP 服务器配置结构体
struct HttpServerConf
{
    std::vector<std::string> address; // 监听地址列表
    int keepalive = 0;                // keep-alive 设置
    int timeout = 1000 * 2 * 60;      // 超时时间，默认 2 分钟
    std::string name;                 // 服务器名称
    int ssl = 0;
    std::string cert_file;
    std::string key_file;

    // 配置是否合法
    bool isValid() const
    {
        return !address.empty();
    }

    // 两个配置是否相等
    bool operator==(const HttpServerConf &oth) const
    {
        return address == oth.address && keepalive == oth.keepalive && timeout == oth.timeout &&
               name == oth.name && ssl == oth.ssl && cert_file == oth.cert_file &&
               key_file == oth.key_file;
    }
};

// YAML 字符串 -> HttpServerConf 对象
template <>
class LexicalCast<std::string, HttpServerConf>
{
public:
    HttpServerConf operator()(const std::string &v)
    {
        YAML::Node node = YAML::Load(v);
        HttpServerConf conf;
        conf.keepalive = node["keepalive"].as<int>(conf.keepalive);
        conf.timeout = node["timeout"].as<int>(conf.timeout);
        conf.name = node["name"].as<std::string>(conf.name);
        conf.ssl = node["ssl"].as<int>(conf.ssl);
        conf.cert_file = node["cert_file"].as<std::string>(conf.cert_file);
        conf.key_file = node["key_file"].as<std::string>(conf.key_file);
        if (node["address"].IsDefined())
        {
            for (size_t i = 0; i < node["address"].size(); ++i)
            {
                conf.address.push_back(node["address"][i].as<std::string>());
            }
        }
        return conf;
    }
};

// HttpServerConf 对象 -> YAML 字符串
template <>
class LexicalCast<HttpServerConf, std::string>
{
public:
    std::string operator()(const HttpServerConf &conf)
    {
        YAML::Node node;
        node["name"] = conf.name;
        node["keepalive"] = conf.keepalive;
        node["timeout"] = conf.timeout;
        node["ssl"] = conf.ssl;
        node["cert_file"] = conf.cert_file;
        node["key_file"] = conf.key_file;
        for (auto &i : conf.address)
        {
            node["address"].push_back(i);
        }
        std::stringstream ss;
        ss << node;
        return ss.str();
    }
};

// 配置项：HTTP 服务器配置列表
static sylar::ConfigVar<std::vector<HttpServerConf>>::ptr g_http_servers_conf =
    sylar::Config::Lookup("http_servers", std::vector<HttpServerConf>(), "http server config");

// Application 单例指针初始化
Application *Application::s_instance = nullptr;

// 构造函数，设置单例
Application::Application()
{
    s_instance = this;
}

// 初始化应用程序
bool Application::init(int argc, char **argv)
{
    m_argc = argc;
    m_argv = argv;

    // 注册命令行参数帮助
    sylar::EnvMgr::GetInstance()->addHelp("s", "start with the terminal");
    sylar::EnvMgr::GetInstance()->addHelp("d", "run as daemon");
    sylar::EnvMgr::GetInstance()->addHelp("c", "conf path default: ./conf");
    sylar::EnvMgr::GetInstance()->addHelp("p", "print help");

    // 初始化环境管理器，解析命令行参数
    if (!sylar::EnvMgr::GetInstance()->init(argc, argv))
    {
        sylar::EnvMgr::GetInstance()->printHelp();
        return false;
    }

    // 打印帮助后退出
    if (sylar::EnvMgr::GetInstance()->has("p"))
    {
        sylar::EnvMgr::GetInstance()->printHelp();
        return false;
    }

    // 确定运行类型：0 未指定，1 前台，2 守护进程
    int run_type = 0;
    if (sylar::EnvMgr::GetInstance()->has("s"))
    {
        run_type = 1;
    }
    if (sylar::EnvMgr::GetInstance()->has("d"))
    {
        run_type = 2;
    }

    // 未指定运行方式，打印帮助
    if (run_type == 0)
    {
        sylar::EnvMgr::GetInstance()->printHelp();
        return false;
    }

    // 获取配置路径，加载配置
    /**
        这里的意思是：
        先拼出 pid 文件路径
        再检查这个 pid 文件里记录的进程号对应的进程是否还活着
        如果活着，说明服务已经在跑，就不允许再启动一个新的实例
        否则可能会把同一个端口绑定两次，或者同一份业务逻辑跑出两个进程，造成冲突。
        而且，守护化以后，进程会 fork，父子进程关系会变化。这个时候如果没有 PID
        文件，外部不容易知道真正提供服务的那个进程号到底是多少。
    */
    std::string conf_path = sylar::EnvMgr::GetInstance()->getAbsolutePath(
        sylar::EnvMgr::GetInstance()->get("c", "conf"));
    SYLAR_LOG_INFO(g_logger) << "load conf path:" << conf_path;
    sylar::Config::LoadFromConfDir(conf_path);

    // 检查服务器是否已经运行
    std::string pidfile = g_server_work_path->getValue() + "/" + g_server_pid_file->getValue();
    if (sylar::FSUtil::IsRunningPidfile(pidfile))
    {
        SYLAR_LOG_ERROR(g_logger) << "server is running:" << pidfile;
        return false;
    }

    // 创建工作目录
    if (!sylar::FSUtil::Mkdir(g_server_work_path->getValue()))
    {
        SYLAR_LOG_FATAL(g_logger) << "create work path [" << g_server_work_path->getValue()
                                  << " errno=" << errno << " errstr=" << strerror(errno);
        return false;
    }
    return true;
}

// 运行应用程序
bool Application::run()
{
    bool is_daemon = sylar::EnvMgr::GetInstance()->has("d"); // 是否以守护进程运行
    return start_daemon(
        m_argc, m_argv,
        std::bind(&Application::main, this, std::placeholders::_1, std::placeholders::_2),
        is_daemon);
}

// 主逻辑
int Application::main(int argc, char **argv)
{
    SYLAR_LOG_INFO(g_logger) << "main";

    // 写 PID 文件
    {
        std::string pidfile = g_server_work_path->getValue() + "/" + g_server_pid_file->getValue();
        std::ofstream ofs(pidfile);
        if (!ofs)
        {
            SYLAR_LOG_ERROR(g_logger) << "open pidfile " << pidfile << " failed";
            return false;
        }
        ofs << getpid();
    }

    // 创建 IOManager 调度器，1 个线程
    sylar::IOManager iom(4);
    iom.schedule(std::bind(&Application::run_fiber, this)); // 调度 run_fiber 协程
    iom.stop();                                             // 等待调度器完成
    return 0;
}

// 运行 Fiber 协程，负责启动 HTTP 服务器
int Application::run_fiber()
{
    auto http_confs = g_http_servers_conf->getValue(); // 获取 HTTP 服务器配置列表
    for (auto &i : http_confs)
    {
        // 打印服务器配置
        SYLAR_LOG_INFO(g_logger) << LexicalCast<HttpServerConf, std::string>()(i);

        std::vector<Address::ptr> address; // 存储解析后的地址
        for (auto &a : i.address)
        {
            size_t pos = a.find(":");
            if (pos == std::string::npos)
            {
                // Unix Socket 地址
                address.push_back(UnixAddress::ptr(new UnixAddress(a)));
                continue;
            }
            // IP:PORT 地址
            int32_t port = atoi(a.substr(pos + 1).c_str());
            auto addr = sylar::IPAddress::Create(a.substr(0, pos).c_str(), port);
            if (addr)
            {
                address.push_back(addr);
                continue;
            }
            // 获取多网卡接口地址
            std::vector<std::pair<Address::ptr, uint32_t>> result;
            if (!sylar::Address::GetInterfaceAddresses(result, a.substr(0, pos)))
            {
                SYLAR_LOG_ERROR(g_logger) << "invalid address: " << a;
                continue;
            }
            for (auto &x : result)
            {
                auto ipaddr = std::dynamic_pointer_cast<IPAddress>(x.first);
                if (ipaddr)
                {
                    ipaddr->setPort(atoi(a.substr(pos + 1).c_str()));
                }
                address.push_back(ipaddr);
            }
        }

        // 创建 HTTP 服务器实例
        sylar::http::HttpServer::ptr server(new sylar::http::HttpServer(i.keepalive));
        auto sd = server->getServletDispatch();
#define XX(...) #__VA_ARGS__
        sd->addServlet("/sylarx/xx",
                       [](sylar::http::HttpRequest::ptr req, sylar::http::HttpResponse::ptr rsp,
                          sylar::http::HttpSession::ptr session)
                       {
                           rsp->setBody(
                               XX(<html><head><title> 404 Not Found</ title></ head><body><center>
                                          <h1> 404 Not Found</ h1></ center><hr><center> nginx /
                                          1.16.0 <
                                      / center > </ body></ html> < !--a padding to disable MSIE and
                                  Chrome friendly error page-- > < !--a padding to disable MSIE and
                                  Chrome friendly error page-- > < !--a padding to disable MSIE and
                                  Chrome friendly error page-- > < !--a padding to disable MSIE and
                                  Chrome friendly error page-- > < !--a padding to disable MSIE and
                                  Chrome friendly error page-- > < !--a padding to disable MSIE and
                                  Chrome friendly error page-- >));
                           return 0;
                       });

        std::vector<Address::ptr> fails;
        if (!server->bind(address, fails, i.ssl)) // 绑定地址
        {
            for (auto &x : fails)
            {
                SYLAR_LOG_ERROR(g_logger) << "bind address fail:" << *x;
            }
            _exit(0); // 绑定失败直接退出
        }
        if (i.ssl)
        {
            if (!server->loadCertificates(i.cert_file, i.key_file))
            {
                SYLAR_LOG_ERROR(g_logger) << "loadCertificates fail, cert_file=" << i.cert_file
                                          << " key_file=" << i.key_file;
            }
        }
        if (!i.name.empty())
        {
            server->setName(i.name); // 设置服务器名称
        }
        server->start();                 // 启动服务器
        m_httpservers.push_back(server); // 保存服务器实例
    }

    // 主循环示例，只是为了让程序活着，如果是更正经的项目，后面应该是类似于这样
    /**
            while (!m_stopping) {
                // 定期健康检查
                // 统计指标
                // 热更新配置
                // 清理资源
                sleep(1);
            }
            // 优雅停机
            for (auto& server : m_httpservers) {
                server->stop();
            }
            // 删除 pid 文件
    */
    while (true)
    {
        SYLAR_LOG_INFO(g_logger) << "application running"; // 测试日志
        usleep(1000 * 1000);
    }
    return 0;
}

} // namespace sylar