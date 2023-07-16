# toolchain.windows_mingw.cmake
# 1. mkdir build && cd build
# 2. cmake .. --no-warn-unused-cli -DCMAKE_BUILD_TYPE:STRING=Debug -DCMAKE_TOOLCHAIN_FILE=%cd%\..\toolchain.linux_x86_64_gcc.cmake
set(CMAKE_TARGET_SYSTEM_NAME linux)
set(CMAKE_TARGET_SYSTEM_PROCESSOR x86_64)
set(CMAKE_TARGET_TOOLCHAIN_NAME gcc)
set(CMAKE_TARGET_BUILD_TYPE Debug)
string(TOLOWER "${CMAKE_BUILD_TYPE}" CMAKE_TARGET_BUILD_TYPE)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 设置C和C++编译器路径
set(CMAKE_C_COMPILER "/usr/bin/gcc")
set(CMAKE_CXX_COMPILER "/usr/bin/g++")

# 设置编译选项
set(CMAKE_C_FLAGS "-Wall")
set(CMAKE_CXX_FLAGS "-Wall")
