cmake_minimum_required(VERSION 3.2)

project(3rdparty)

set(3RDPARTY_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(3RDPARTY_INSTALL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/dist/windows_amd64_gcc)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_VERBOSE_MAKEFILE ON)

include(${CMAKE_CURRENT_LIST_DIR}/../../Option.cmake)
message(STATUS "3RDPARTY_DIR: ${3RDPARTY_DIR}")

add_subdirectory(${3RDPARTY_DIR}/g3log)

add_subdirectory(${3RDPARTY_DIR}/zlib)

add_subdirectory(${3RDPARTY_DIR}/g3sinks)