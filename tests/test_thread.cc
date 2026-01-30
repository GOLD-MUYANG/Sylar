#include "sylar/config.h"
#include "sylar/log.h"
#include "sylar/sylar.h"
#include <atomic>
#include <chrono>
#include <unistd.h>
sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");
std::atomic<int> count(0);
// sylar::RWMutex s_mutex;
sylar::Mutex s_mutex;

void fun1()
{
    SYLAR_LOG_INFO(g_logger) << "name: " << sylar::Thread::GetName()
                             << " this.name: " << sylar::Thread::GetThis()->getName()
                             << " id: " << sylar::GetThreadId()
                             << " this.id: " << sylar::Thread::GetThis()->getId();

    for (int i = 0; i < 100000; ++i)
    {
        // sylar::RWMutex::WriteLock lock(s_mutex);
        sylar::Mutex::Lock lock(s_mutex);
        ++count;
    }
}
void fun2() {}

// 2. 补全缺失的fun1函数（日志输出核心逻辑，每个线程执行指定次数日志写入）
void withMutexFileLogOut()
{
    // 每个线程执行500次日志输出，10个线程总计 10*500=5000次
    const int per_thread_log_count = 5000;
    for (int i = 0; i < per_thread_log_count; ++i)
    {
        // 输出日志（包含线程名、当前计数、循环次数）
        SYLAR_LOG_INFO(g_logger) << "thread: " << sylar::Thread::GetName()
                                 << " | current count: " << count << " | loop index: " << i;
        // 原子操作自增，保证计数准确
        count++;
    }
}

// 4. 主函数：配置日志（FileLogAppender）、创建线程、计时、输出结果
int testMutex()
{
    // ===== 步骤2：记录开始时间（使用std::chrono高精度计时） =====
    // 记录当前时间点（毫秒级精度，足够满足日志效率测试）
    auto start_time = std::chrono::high_resolution_clock::now();

    std::cout << "=====================================" << std::endl;
    // ===== 步骤3：创建线程，执行日志输出任务 =====
    SYLAR_LOG_INFO(g_logger) << "thread test begin";
    std::vector<sylar::Thread::ptr> thrs;

    // 创建10个线程（你原代码的逻辑：循环10次，每次创建2个线程）
    for (int i = 0; i < 10; ++i)
    {
        sylar::Thread::ptr thr(
            new sylar::Thread(&withMutexFileLogOut, "name_" + std::to_string(i * 2)));
        thrs.push_back(thr);
    }

    // 等待所有线程执行完毕
    for (size_t i = 0; i < thrs.size(); ++i)
    {
        thrs[i]->join();
    }

    // ===== 步骤4：记录结束时间，计算耗时 =====
    auto end_time = std::chrono::high_resolution_clock::now();
    // 计算时间差（转换为毫秒）
    auto duration_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    // 可选：转换为微秒（更高精度，看日志写入速度）
    auto duration_us =
        std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();

    // ===== 步骤5：输出测试结果 =====
    SYLAR_LOG_INFO(g_logger) << "thread test end";
    SYLAR_LOG_INFO(g_logger) << "total log count=" << count;

    // 控制台输出耗时（更直观，避免日志文件读取麻烦）
    std::cout << "=====================================" << std::endl;
    std::cout << "测试结果统计：" << std::endl;
    std::cout << "1. 总日志写入次数：" << count << " 次" << std::endl;
    std::cout << "2. 总耗时：" << duration_ms << " 毫秒（" << duration_us / 1000.0 << " 微秒）"
              << std::endl;
    std::cout << "3. 平均每次日志写入耗时：" << (double)duration_us / count << " 微秒" << std::endl;
    std::cout << "=====================================" << std::endl;

    //加普通锁，总耗时4058ms
    //加NullMutex（也就是不加锁），出现了串行的问题（多线程竞争写），总耗时3998ms
    //加Spinlock 耗时8000ms，原因是有频繁的io操作，导致自旋锁一直在占用cpu资源，反而比互斥锁慢
    return 0;
}

int main()
{
    // SYLAR_LOG_INFO(g_logger) << "thread test begin";
    // std::vector<sylar::Thread::ptr> thrs;
    // for (int i = 0; i < 10; ++i)
    // {
    //     sylar::Thread::ptr thr(new sylar::Thread(&fun1, "name_" + std::to_string(i * 2)));
    //     sylar::Thread::ptr thr2(new sylar::Thread(&fun1, "name_" + std::to_string(i * 2 + 1)));
    //     thrs.push_back(thr);
    //     thrs.push_back(thr2);
    // }

    // for (size_t i = 0; i < thrs.size(); ++i)
    // {
    //     thrs[i]->join();
    // }
    // SYLAR_LOG_INFO(g_logger) << "thread test end";
    // SYLAR_LOG_INFO(g_logger) << "count=" << count;

    YAML::Node root = YAML::LoadFile("/home/sylar/workspace/sylar/bin/conf/log2.yml");
    sylar::Config::LoadFromYaml(root);
    testMutex();

    return 0;
}
