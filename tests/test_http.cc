#include "sylar/http/http.h"
#include "sylar/log.h"
#include <iostream>

sylar::Logger::ptr logger = SYLAR_LOG_ROOT();

void test_request()
{
    sylar::http::HttpRequest::ptr req(new sylar::http::HttpRequest);
    req->setHeader("host", "www.baidu.com");
    req->setBody("hello world");
    req->dump(std::cout) << std::endl;
}

void test_responce()
{
    sylar::http::HttpResponse::ptr resp(new sylar::http::HttpResponse);
    resp->setHeader("host", "www.baidu.com");
    resp->setBody("hello world");
    resp->setStatus((sylar::http::HttpStatus)400);
    resp->setClose(false);
    resp->dump(std::cout) << std::endl;
}

int main(int argc, char **argv)
{
    test_request();
    test_responce();
}