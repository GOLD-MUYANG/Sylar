cmake_minimum_required(VERSION 3.5)
project(sylar)

include (cmake/utils.cmake)

set(CMAKE_VERBOSE_MAKEFILE ON)
# set(CMAKE_BUILD_TYPE "Debug") #生成可以debug的可执行文件
# set(CMAKE_CXX_FLAGS "$ENV{CXXFLAGS} -rdynamic -O0 -ggdb -std=c++11 -Wall -Wno-deprecated -Werror -Wno-unused-function -Wno-builtin-macro-redefined")

set(CMAKE_BUILD_TYPE "Release")
set(CMAKE_CXX_FLAGS "$ENV{CXXFLAGS} -rdynamic -O3 -DNDEBUG -std=c++11 -Wall -Wno-deprecated -Wno-unused-function -Wno-builtin-macro-redefined")
include_directories(.)
set(LIB_SRC
    sylar/log.cc
    sylar/util.cc
    sylar/config.cc
    sylar/thread.cc
    sylar/fiber.cc
    sylar/scheduler.cc
    sylar/iomanager.cc
    sylar/timer.cc
    sylar/hook.cc
    sylar/fd_manager.cc
    sylar/address.cc
    sylar/socket.cc
    sylar/bytearray.cc
    sylar/http/http.cc
    sylar/http/http11_parser.rl.cc
    sylar/http/httpclient_parser.rl.cc
    sylar/http/http_parser.cc
    sylar/tcp_server.cc
    sylar/socket_stream.cc
    sylar/stream.cc
    sylar/http/http_session.cc
    sylar/http/http_server.cc
    sylar/http/servlet.cc
    sylar/http/http_connection.cc
    sylar/uri.rl.cc
    sylar/daemon.cc
    sylar/env.cc
    sylar/application.cc
    sylar/util/hash_util.cc
    sylar/http/ws_connection.cc
    sylar/http/ws_session.cc
    sylar/http/ws_server.cc
    sylar/http/ws_servlet.cc
    sylar/mutex.cc
    sylar/worker.cc
    sylar/library.cc
    sylar/main.cc
    sylar/module.cc
    )


ragelmaker(sylar/http/http11_parser.rl LIB_SRC ${CMAKE_CURRENT_SOURCE_DIR}/sylar/http)
ragelmaker(sylar/http/httpclient_parser.rl LIB_SRC ${CMAKE_CURRENT_SOURCE_DIR}/sylar/http)
ragelmaker(sylar/uri.rl LIB_SRC ${CMAKE_CURRENT_SOURCE_DIR}/sylar)

add_library(sylar SHARED ${LIB_SRC})
force_redefine_file_macro_for_sources(sylar)    #__FILE__重定义
#add_library(sylar_static STATIC ${LIB_SRC})
#SET_TARGET_PROPERTIES (sylar_static PROPERTIES OUTPUT_NAME "sylar")
target_link_libraries(sylar PUBLIC yaml-cpp)


set(LIBS
        sylar
        dl
        pthread
        yaml-cpp
        ssl
        crypto)

# sylar_add_executable(test1 "tests/test.cc" sylar "${LIBS}")
# sylar_add_executable(test_config "tests/test_config.cc" sylar "${LIBS}")
# sylar_add_executable(test_thread "tests/test_thread.cc" sylar "${LIBS}")
# sylar_add_executable(test_util "tests/test_util.cc" sylar "${LIBS}")
# sylar_add_executable(test_fiber "tests/test_fiber.cc" sylar "${LIBS}")
# sylar_add_executable(test_scheduler "tests/test_scheduler.cc" sylar "${LIBS}")
# sylar_add_executable(test_iomanager "tests/test_iomanager.cc" sylar "${LIBS}")
# sylar_add_executable(test_hook "tests/test_hook.cc" sylar "${LIBS}")
# sylar_add_executable(test_address "tests/test_address.cc" sylar "${LIBS}")
# sylar_add_executable(test_socket "tests/test_socket.cc" sylar "${LIBS}")
# sylar_add_executable(test_bytearray "tests/test_bytearray.cc" sylar "${LIBS}")
# sylar_add_executable(test_http "tests/test_http.cc" sylar "${LIBS}")
# sylar_add_executable(test_http_parser "tests/test_http_parser.cc" sylar "${LIBS}")
# sylar_add_executable(test_tcp_server "tests/test_tcp_server.cc" sylar "${LIBS}")
# sylar_add_executable(echo_server "examples/echo_server.cc" sylar "${LIBS}")
# sylar_add_executable(test_http_server "tests/test_http_server.cc" sylar "${LIBS}")
sylar_add_executable(test_http_connection "tests/test_http_connection.cc" sylar "${LIBS}")
# sylar_add_executable(test_uri "tests/test_uri.cc" sylar "${LIBS}")
# sylar_add_executable(test_daemon "tests/test_daemon.cc" sylar "${LIBS}")
# sylar_add_executable(test_application "tests/test_application.cc" sylar "${LIBS}")
# sylar_add_executable(test_ws_server "tests/test_ws_server.cc" sylar "${LIBS}")
# sylar_add_executable(test_ws_client "tests/test_ws_client.cc" sylar "${LIBS}")
sylar_add_executable(bin_sylar "sylar/main.cc" sylar "${LIBS}")
set_target_properties(bin_sylar PROPERTIES OUTPUT_NAME "sylar")

add_library(module SHARED tests/test_module.cc)
target_link_libraries(module PRIVATE sylar)
set_target_properties(module PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin/module
)
add_dependencies(bin_sylar module)

# add_executable(test tests/test.cc)
# add_dependencies(test sylar)
# force_redefine_file_macro_for_sources(test)    #__FILE__重定义
# target_link_libraries(test sylar)

SET(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/bin)
SET(LIBRARY_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/lib)