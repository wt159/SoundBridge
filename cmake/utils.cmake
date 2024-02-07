set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

message(STATUS "CMAKE_SYSTEM_NAME: ${CMAKE_SYSTEM_NAME}")
message(STATUS "CMAKE_SYSTEM_PROCESSOR: ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "CMAKE_BUILD_TYPE: ${CMAKE_BUILD_TYPE}")

if(NOT CMAKE_TARGET_SYSTEM_NAME)
    set(CMAKE_TARGET_SYSTEM_NAME ${CMAKE_SYSTEM_NAME})
    string(TOLOWER "${CMAKE_SYSTEM_NAME}" CMAKE_TARGET_SYSTEM_NAME)
endif()

if(NOT CMAKE_TARGET_SYSTEM_PROCESSOR)
    set(CMAKE_TARGET_SYSTEM_PROCESSOR x86_64)
endif()

if(NOT CMAKE_TARGET_BUILD_TYPE)
    set(CMAKE_TARGET_BUILD_TYPE ${CMAKE_SYSTEM_NAME})
    string(TOLOWER "${CMAKE_BUILD_TYPE}" CMAKE_TARGET_BUILD_TYPE)
endif()

if(NOT CMAKE_TARGET_TOOLCHAIN_NAME)
    set(CMAKE_TARGET_TOOLCHAIN_NAME gcc)
endif()

set(CMAKE_TARGET_3RDPARTY_NAME ${CMAKE_TARGET_SYSTEM_NAME}_${CMAKE_TARGET_SYSTEM_PROCESSOR}_${CMAKE_TARGET_TOOLCHAIN_NAME}_${CMAKE_TARGET_BUILD_TYPE})
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)


if(CMAKE_BUILD_TYPE MATCHES "Debug")
    set(CMAKE_VERBOSE_MAKEFILE OFF)
    set(COMMON_FLAGS "-O2 -DEXE_NAME=SoundBridge")
else()
    set(CMAKE_VERBOSE_MAKEFILE OFF)
    set(COMMON_FLAGS "-O3 -DEXE_NAME=SoundBridge")
endif()

# 检查C++编译器版本是否符合要求
if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "8.0.0")
    message(WARNING "C++ compiler version (${CMAKE_CXX_COMPILER_VERSION})")
else()
    list(APPEND COMMON_FLAGS "-Wno-format")
    
endif()
message(INFO "common flags: ${COMMON_FLAGS}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${COMMON_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${COMMON_FLAGS}")
