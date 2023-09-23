cmake_minimum_required(VERSION 3.2)

project(SoundBridge)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/utils.cmake)

set(ROOT_DIR ${CMAKE_CURRENT_LIST_DIR})
message(STATUS "CMAKE_TOOLCHAIN_FILE: ${CMAKE_TOOLCHAIN_FILE}")
set(SDK_DIR ${CMAKE_CURRENT_LIST_DIR}/sdk)
add_subdirectory(${SDK_DIR})