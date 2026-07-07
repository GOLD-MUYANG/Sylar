# AI gateway 构建目标：模块库、示例程序和相关测试。

set(AI_GATEWAY_PROTOCOL_SRC
    modules/ai_gateway/ai_gateway_protocol.cc)

set(AI_GATEWAY_SERVLET_SRC
    modules/ai_gateway/ai_gateway_protocol.cc
    modules/ai_gateway/ai_gateway_servlet.cc)

set(AI_GATEWAY_LOAD_BALANCE_SRC
    modules/ai_gateway/ai_gateway_protocol.cc
    modules/ai_gateway/ai_gateway_servlet.cc
    modules/ai_gateway/ai_gateway_upstream.cc)

set(AI_GATEWAY_REAL_PROVIDER_SRC
    modules/ai_gateway/ai_gateway_protocol.cc
    modules/ai_gateway/real_provider_gateway.cc
    modules/ai_gateway/ai_provider_adapter.cc)

set(AI_GATEWAY_REAL_SMOKE_SRC
    modules/ai_gateway/ai_gateway_protocol.cc
    modules/ai_gateway/real_provider_gateway.cc
    modules/ai_gateway/ai_provider_adapter.cc
    modules/ai_gateway/real_provider_smoke.cc)

set(AI_GATEWAY_REAL_RUNTIME_SRC
    modules/ai_gateway/ai_gateway_protocol.cc
    modules/ai_gateway/real_provider_gateway.cc
    modules/ai_gateway/ai_provider_adapter.cc
    modules/ai_gateway/real_provider_runtime.cc)

set(AI_GATEWAY_MODULE_SRC
    modules/ai_gateway/ai_gateway_module.cc
    modules/ai_gateway/ai_gateway_protocol.cc
    modules/ai_gateway/ai_gateway_demo_servlet.cc
    modules/ai_gateway/ai_gateway_servlet.cc
    modules/ai_gateway/ai_gateway_status_servlet.cc
    modules/ai_gateway/ai_provider_adapter.cc
    modules/ai_gateway/real_provider_gateway.cc
    modules/ai_gateway/real_provider_runtime.cc
    modules/ai_gateway/ai_gateway_upstream.cc)

sylar_add_test_executable(test_ai_gateway_route_registry "tests/test_ai_gateway_route_registry.cc")

sylar_add_jsoncpp_executable(test_ai_gateway_protocol
    "tests/test_ai_gateway_protocol.cc;${AI_GATEWAY_PROTOCOL_SRC}")

sylar_add_jsoncpp_executable(test_ai_gateway_servlet
    "tests/test_ai_gateway_servlet.cc;${AI_GATEWAY_SERVLET_SRC}")

sylar_add_jsoncpp_executable(test_ai_gateway_load_balance
    "tests/test_ai_gateway_load_balance.cc;${AI_GATEWAY_LOAD_BALANCE_SRC}")

sylar_add_jsoncpp_executable(test_ai_gateway_real_provider
    "tests/test_ai_gateway_real_provider.cc;${AI_GATEWAY_REAL_PROVIDER_SRC}")

sylar_add_jsoncpp_executable(test_ai_gateway_real_provider_smoke
    "tests/test_ai_gateway_real_provider_smoke.cc;${AI_GATEWAY_REAL_SMOKE_SRC}")
set_target_properties(test_ai_gateway_real_provider_smoke PROPERTIES EXCLUDE_FROM_ALL TRUE)

sylar_add_jsoncpp_executable(test_ai_gateway_real_runtime
    "tests/test_ai_gateway_real_runtime.cc;${AI_GATEWAY_REAL_RUNTIME_SRC}")

sylar_add_jsoncpp_executable(ai_gateway_real_provider_smoke
    "examples/ai_gateway_real_provider_smoke.cc;${AI_GATEWAY_REAL_SMOKE_SRC}")
set_target_properties(ai_gateway_real_provider_smoke PROPERTIES EXCLUDE_FROM_ALL TRUE)

sylar_add_jsoncpp_executable(test_ai_gateway_status
    "tests/test_ai_gateway_status.cc;modules/ai_gateway/ai_gateway_status_servlet.cc;modules/ai_gateway/ai_gateway_upstream.cc")

sylar_add_test_executable(test_ai_gateway_demo_servlet
    "tests/test_ai_gateway_demo_servlet.cc;modules/ai_gateway/ai_gateway_demo_servlet.cc")

sylar_add_jsoncpp_executable(mock_model_provider
    "examples/mock_model_provider.cc;${AI_GATEWAY_PROTOCOL_SRC}")

add_library(ai_gateway_module SHARED ${AI_GATEWAY_MODULE_SRC})
sylar_target_use_jsoncpp(ai_gateway_module)
target_link_libraries(ai_gateway_module PRIVATE sylar ${JSONCPP_LIBRARIES})
set_target_properties(ai_gateway_module PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin/module)
