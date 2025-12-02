#ifndef __SYLAR_UTIL_H__
#define __SYLAR_UTIL_H__

#include <cstdint>
#include <ctime>
#include <pthread.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

namespace sylar
{
pid_t GetThreadId();
uint32_t GetFiberId();
} // namespace sylar
#endif