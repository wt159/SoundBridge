cmake_minimum_required(VERSION 3.2)

project(sdk)

set(SDK_ROOT_DIR ${CMAKE_CURRENT_LIST_DIR})
message(STATUS "3RDPARTY_DIST_DIR: ${3RDPARTY_DIST_DIR}")
set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH} ${3RDPARTY_DIST_DIR})

# 指定cosmos路径
include_directories(${SDK_ROOT_DIR}/cosmos)
# include_directories(${SDK_ROOT_DIR}/log)
# include_directories(${SDK_ROOT_DIR}/audio/common)
# include_directories(${SDK_ROOT_DIR}/audio/device)
# include_directories(${SDK_ROOT_DIR}/audio/decode)
# include_directories(${SDK_ROOT_DIR}/audio/resample)
# include_directories(${SDK_ROOT_DIR}/extractor)
# include_directories(${SDK_ROOT_DIR})

add_subdirectory(${SDK_ROOT_DIR}/utils)

if(CMAKE_TARGET_SYSTEM_PROCESSOR STREQUAL "arm_gnueabihf")
    set(LOG_WRAPPER_DIR ${SDK_ROOT_DIR}/log/boost_log)
else()
    set(LOG_WRAPPER_DIR ${SDK_ROOT_DIR}/log)
endif()
add_subdirectory(${LOG_WRAPPER_DIR})

add_subdirectory(${SDK_ROOT_DIR}/audio)

add_subdirectory(${SDK_ROOT_DIR}/extractor)

set(SDK_SOURCES MusicPlayList.cpp MusicPlayer.cpp)

add_library(sdk ${SDK_SOURCES})

target_link_libraries(sdk Extractor AudioDevice AudioResample)

target_include_directories(sdk PUBLIC ${SDK_ROOT_DIR})

# add_subdirectory(${SDK_ROOT_DIR}/test)