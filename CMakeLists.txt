cmake_minimum_required(VERSION 3.5)
project(sylar)

include (cmake/utils.cmake)

set(CMAKE_BUILD_TYPE "Debug") #生成可以debug的可执行文件
set(CMAKE_VERBOSE_MAKEFILE ON)
set(CMAKE_CXX_FLAGS "$ENV{CXXFLAGS} -rdynamic -O0 -ggdb -std=c++11 -Wall -Wno-deprecated -Werror -Wno-unused-function -Wno-builtin-macro-redefined")

include_directories(.)
set(LIB_SRC
    sylar/log.cc
    sylar/util.cc
    sylar/config.cc
    sylar/thread.cc
    sylar/fiber.cc
    )

add_library(sylar SHARED ${LIB_SRC})
force_redefine_file_macro_for_sources(sylar)    #__FILE__重定义
#add_library(sylar_static STATIC ${LIB_SRC})
#SET_TARGET_PROPERTIES (sylar_static PROPERTIES OUTPUT_NAME "sylar")
target_link_libraries(sylar PUBLIC yaml-cpp)

add_executable(test tests/test.cc)
add_dependencies(test sylar)
force_redefine_file_macro_for_sources(test)    #__FILE__重定义
target_link_libraries(test sylar)

add_executable(test_config tests/test_config.cc)
add_dependencies(test_config sylar)
force_redefine_file_macro_for_sources(test_config)    #__FILE__重定义
target_link_libraries(test_config sylar yaml-cpp)

add_executable(test_thread tests/test_thread.cc)
add_dependencies(test_thread sylar)
force_redefine_file_macro_for_sources(test_thread)    #__FILE__重定义
target_link_libraries(test_thread sylar yaml-cpp)

add_executable(test_util tests/test_util.cc)
add_dependencies(test_util sylar)
force_redefine_file_macro_for_sources(test_util)    #__FILE__重定义
target_link_libraries(test_util sylar yaml-cpp)

add_executable(test_fiber tests/test_fiber.cc)
add_dependencies(test_fiber sylar)
force_redefine_file_macro_for_sources(test_fiber)    #__FILE__重定义
target_link_libraries(test_fiber sylar yaml-cpp)

SET(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/bin)
SET(LIBRARY_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/lib)
