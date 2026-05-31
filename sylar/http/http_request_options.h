#ifndef __SYLAR_HTTP_REQUEST_OPTIONS_H__
#define __SYLAR_HTTP_REQUEST_OPTIONS_H__

#include <stdint.h>

namespace sylar
{
namespace http
{

/**
 * @brief HTTP客户端请求超时参数
 */
struct HttpRequestOptions
{
    static HttpRequestOptions FromTimeout(uint64_t timeout_ms);

    uint64_t connect_timeout_ms = (uint64_t)-1;
    uint64_t send_timeout_ms = (uint64_t)-1;
    uint64_t recv_timeout_ms = (uint64_t)-1;
    uint64_t total_timeout_ms = (uint64_t)-1;
};

} // namespace http
} // namespace sylar

#endif
