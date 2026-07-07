cmake_minimum_required(VERSION 3.5)
project(sylar)
enable_testing()

include(cmake/utils.cmake)

find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONCPP REQUIRED jsoncpp)

set(CMAKE_VERBOSE_MAKEFILE ON)
# set(CMAKE_BUILD_TYPE "Debug") #生成可以debug的可执行文件
# set(CMAKE_CXX_FLAGS "$ENV{CXXFLAGS} -rdynamic -O0 -ggdb -std=c++11 -Wall -Wno-deprecated -Werror -Wno-unused-function -Wno-builtin-macro-redefined")

set(CMAKE_BUILD_TYPE "Release")
set(CMAKE_CXX_FLAGS "$ENV{CXXFLAGS} -rdynamic -O3 -DNDEBUG -std=c++11 -Wall -Wno-deprecated -Wno-unused-function -Wno-builtin-macro-redefined")

include_directories(.)

include(cmake/sylar_core.cmake)

set(LIBS
        sylar
        dl
        pthread
        yaml-cpp
        ssl
        crypto)

include(cmake/ai_gateway.cmake)

sylar_add_executable(bin_sylar "sylar/main.cc" sylar "${LIBS}")
set_target_properties(bin_sylar PROPERTIES OUTPUT_NAME "sylar")
add_dependencies(bin_sylar ai_gateway_module)

add_library(module SHARED tests/test_module.cc)
target_link_libraries(module PRIVATE sylar)
set_target_properties(module PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin/module
)
add_dependencies(bin_sylar module)

include(cmake/sylar_tests.cmake)

# add_executable(test tests/test.cc)
# add_dependencies(test sylar)
# force_redefine_file_macro_for_sources(test)    #__FILE__重定义
# target_link_libraries(test sylar)

SET(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/bin)
SET(LIBRARY_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/lib)
