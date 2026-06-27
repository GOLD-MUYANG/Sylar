#ifndef __SYLAR_HTTP_REQUEST_OPTIONS_H__
#define __SYLAR_HTTP_REQUEST_OPTIONS_H__

#include <stdint.h>

#include <string>

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

    // HTTPS 客户端验证配置。server_name 为空时，HTTP 连接层使用目标 host
    // 作为 SNI 和主机名校验名称；生产真实 Provider 不应关闭 verify_peer。
    std::string tls_server_name;
    std::string tls_ca_file;
    std::string tls_ca_path;
    bool tls_verify_peer = true;
};

} // namespace http
} // namespace sylar

#endif
