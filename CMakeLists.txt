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
        yaml-cpp)

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
sylar_add_executable(test_http_server "tests/test_http_server.cc" sylar "${LIBS}")
# sylar_add_executable(test_http_connection "tests/test_http_connection.cc" sylar "${LIBS}")
# sylar_add_executable(test_uri "tests/test_uri.cc" sylar "${LIBS}")
# sylar_add_executable(test_daemon "tests/test_daemon.cc" sylar "${LIBS}")
sylar_add_executable(test_application "tests/test_application.cc" sylar "${LIBS}")



# add_executable(test tests/test.cc)
# add_dependencies(test sylar)
# force_redefine_file_macro_for_sources(test)    #__FILE__重定义
# target_link_libraries(test sylar)

# add_executable(test_config tests/test_config.cc)
# add_dependencies(test_config sylar)
# force_redefine_file_macro_for_sources(test_config)    #__FILE__重定义
# target_link_libraries(test_config sylar yaml-cpp)

# add_executable(test_thread tests/test_thread.cc)
# add_dependencies(test_thread sylar)
# force_redefine_file_macro_for_sources(test_thread)    #__FILE__重定义
# target_link_libraries(test_thread sylar yaml-cpp)

# add_executable(test_util tests/test_util.cc)
# add_dependencies(test_util sylar)
# force_redefine_file_macro_for_sources(test_util)    #__FILE__重定义
# target_link_libraries(test_util sylar yaml-cpp)

# add_executable(test_fiber tests/test_fiber.cc)
# add_dependencies(test_fiber sylar)
# force_redefine_file_macro_for_sources(test_fiber)    #__FILE__重定义
# target_link_libraries(test_fiber sylar yaml-cpp)

# add_executable(test_scheduler tests/test_scheduler.cc)
# add_dependencies(test_scheduler sylar)
# force_redefine_file_macro_for_sources(test_scheduler)    #__FILE__重定义
# target_link_libraries(test_scheduler sylar yaml-cpp)

# add_executable(test_iomanager tests/test_iomanager.cc)
# add_dependencies(test_iomanager sylar)
# force_redefine_file_macro_for_sources(test_iomanager)    #__FILE__重定义
# target_link_libraries(test_iomanager sylar yaml-cpp)

# add_executable(test_hook tests/test_hook.cc)
# add_dependencies(test_hook sylar)
# force_redefine_file_macro_for_sources(test_hook)    #__FILE__重定义
# target_link_libraries(test_hook sylar yaml-cpp)

# add_executable(test_address tests/test_address.cc)
# add_dependencies(test_address sylar)
# force_redefine_file_macro_for_sources(test_address)    #__FILE__重定义
# target_link_libraries(test_address sylar yaml-cpp)

# add_executable(test_socket tests/test_socket.cc)
# add_dependencies(test_socket sylar)
# force_redefine_file_macro_for_sources(test_socket)    #__FILE__重定义
# target_link_libraries(test_socket sylar yaml-cpp)

# add_executable(test_bytearray tests/test_bytearray.cc)
# add_dependencies(test_bytearray sylar)
# force_redefine_file_macro_for_sources(test_bytearray)    #__FILE__重定义
# target_link_libraries(test_bytearray sylar yaml-cpp)

# add_executable(test_http tests/test_http.cc)
# add_dependencies(test_http sylar)
# force_redefine_file_macro_for_sources(test_http)    #__FILE__重定义
# target_link_libraries(test_http sylar yaml-cpp)

# add_executable(test_http_parser tests/test_http_parser.cc)
# add_dependencies(test_http_parser sylar)
# force_redefine_file_macro_for_sources(test_http_parser)    #__FILE__重定义
# target_link_libraries(test_http_parser sylar yaml-cpp)

# add_executable(test_tcp_server tests/test_tcp_server.cc)
# add_dependencies(test_tcp_server sylar)
# force_redefine_file_macro_for_sources(test_tcp_server)    #__FILE__重定义
# target_link_libraries(test_tcp_server sylar yaml-cpp)


# add_executable(echo_server examples/echo_server.cc)
# add_dependencies(echo_server sylar)
# force_redefine_file_macro_for_sources(echo_server)    #__FILE__重定义
# target_link_libraries(echo_server sylar yaml-cpp)




SET(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/bin)
SET(LIBRARY_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/lib)