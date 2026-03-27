cmake_minimum_required(VERSION 3.2)

project(sdk)

set(SDK_ROOT_DIR ${CMAKE_CURRENT_LIST_DIR})
message(STATUS "3RDPARTY_DIST_DIR: ${3RDPARTY_DIST_DIR}")
set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH} ${3RDPARTY_DIST_DIR})

# 指定cosmos路径
include_directories(${SDK_ROOT_DIR}/cosmos)

add_subdirectory(${SDK_ROOT_DIR}/utils)

set(LOG_WRAPPER_DIR ${SDK_ROOT_DIR}/log/boost_log)
add_subdirectory(${LOG_WRAPPER_DIR})

add_subdirectory(${SDK_ROOT_DIR}/audio)

add_subdirectory(${SDK_ROOT_DIR}/extractor)

# Legacy SDK sources (internal use)
set(SDK_LEGACY_SOURCES
    MusicPlayList.cpp
    MusicPlayer.cpp
    LogApi.cpp
    ErrorCode.cpp
    Metrics.cpp
)

# New public API sources
set(SDK_PUBLIC_SOURCES
    version.cpp
    error_impl.cpp
    logging_impl.cpp
    player_impl.cpp
)

add_library(sdk ${SDK_LEGACY_SOURCES} ${SDK_PUBLIC_SOURCES})

target_link_libraries(sdk Extractor AudioDevice AudioResample LogWrapper)

# Public include directory for new API
target_include_directories(sdk
    PUBLIC
        $<BUILD_INTERFACE:${SDK_ROOT_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${SDK_ROOT_DIR}
)

if(BUILD_TESTING)
    add_subdirectory(${SDK_ROOT_DIR}/test)
endif()
