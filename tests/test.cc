#include "./sylar/log.h"
#include "./sylar/util.h"
#include <cstdint>
#include <iostream>
#include <thread>
using namespace std;
int main()
{
    sylar::Logger::ptr logger(new sylar::Logger);
    logger->addAppender(sylar::StdoutLogAppender::ptr(new sylar::StdoutLogAppender));

    // sylar::LogEvent::ptr event(new sylar::LogEvent(__FILE__, __LINE__, 0, sylar::GetThreadId(),
    // sylar::GetFiberId(), time(0)));

    // logger->log(sylar::LogLevel::DEBUG, event);
    cout << "helle world" << endl;

    SYLAR_LOG_INFO(logger) << "test macro";
    SYLAR_LOG_ERROR(logger) << "test macro error";
    return 0;
}