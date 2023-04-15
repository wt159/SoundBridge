cmake_minimum_required(VERSION 3.2)

set(3RDPARTY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty)
set(3RDPARTY_INSTALL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/install)

message(STATUS "3RDPARTY_DIR: ${3RDPARTY_DIR}")

add_subdirectory(${3RDPARTY_DIR}/g3log)

add_subdirectory(${3RDPARTY_DIR}/zlib)

add_subdirectory(${3RDPARTY_DIR}/g3sinks)