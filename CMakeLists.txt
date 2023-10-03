cmake_minimum_required(VERSION 3.2)

project(SoundBridge_PROJECT)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/utils.cmake)

set(ROOT_DIR ${CMAKE_CURRENT_LIST_DIR})
message(STATUS "CMAKE_TOOLCHAIN_FILE: ${CMAKE_TOOLCHAIN_FILE}")
set(SDK_DIR ${CMAKE_CURRENT_LIST_DIR}/sdk)
set(3RDPARTY_DIST_DIR ${SDK_DIR}/3rdparty/dist/${CMAKE_TARGET_3RDPARTY_NAME})
add_subdirectory(${SDK_DIR})

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/app)