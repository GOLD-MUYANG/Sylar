#include "http_request_options.h"

namespace sylar
{
namespace http
{

HttpRequestOptions HttpRequestOptions::FromTimeout(uint64_t timeout_ms)
{
    HttpRequestOptions options;
    options.connect_timeout_ms = timeout_ms;
    options.send_timeout_ms = timeout_ms;
    options.recv_timeout_ms = timeout_ms;
    options.total_timeout_ms = timeout_ms;
    return options;
}

} // namespace http
} // namespace sylar
