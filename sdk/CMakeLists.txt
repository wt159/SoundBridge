cmake_minimum_required(VERSION 3.2)

project(sdk)

set(SDK_ROOT_DIR ${CMAKE_CURRENT_LIST_DIR})
set(3RDPARTY_DIST_DIR ${SDK_ROOT_DIR}/3rdparty/dist/${CMAKE_TARGET_3RDPARTY_NAME})
message(STATUS "3RDPARTY_DIST_DIR: ${3RDPARTY_DIST_DIR}")
set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH} ${3RDPARTY_DIST_DIR})

# 指定cosmos路径
include_directories(${SDK_ROOT_DIR}/cosmos)
include_directories(${SDK_ROOT_DIR}/log)

set(LOG_WRAPPER_DIR ${SDK_ROOT_DIR}/log)
add_subdirectory(${LOG_WRAPPER_DIR})

add_subdirectory(${SDK_ROOT_DIR}/audio)

add_subdirectory(${SDK_ROOT_DIR}/extractor)

add_subdirectory(${SDK_ROOT_DIR}/test)