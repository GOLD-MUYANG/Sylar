#include <./sylar/log.h>
#include <iostream>
using namespace std;
int main()
{
    sylar::Logger::ptr logger(new sylar::Logger);
    logger->addAppender(sylar::StdoutLogAppender::ptr(new sylar::StdoutLogAppender));

    sylar::LogEvent::ptr event(new sylar::LogEvent(__FILE__, __LINE__, 0, 1, 2, time(0)));

    logger->log(sylar::LogLevel::DEBUG, event);
    cout << "helle world" << endl;
    return 0;
}