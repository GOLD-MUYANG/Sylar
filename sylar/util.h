#ifndef __SYLAR_UTIL_H__
#define __SYLAR_UTIL_H__

#include <cstdint>
#include <ctime>
#include <pthread.h>
#include <stdio.h>
#include <string>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace sylar
{
pid_t GetThreadId();
uint32_t GetFiberId();

void BackTrace(std::vector<std::string> &bt, int size, int skip = 1);

std::string BacktraceToString(int size, int skip = 1, const std::string &prefix = "");

uint32_t GetFiberID();
} // namespace sylar
#endif