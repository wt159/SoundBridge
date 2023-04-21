# toolchain.windows_mingw.cmake
# 1. mkdir build && cd build
# 2. cmake .. --no-warn-unused-cli -DCMAKE_BUILD_TYPE:STRING=Debug -DCMAKE_TOOLCHAIN_FILE=%cd%\..\toolchain.windows_mingw.cmake -G "MinGW Makefiles"
set(CMAKE_TARGET_SYSTEM_NAME linux)
set(CMAKE_TARGET_SYSTEM_PROCESSOR amd64)
set(CMAKE_TARGET_TOOLCHAIN_NAME gcc)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 设置C和C++编译器路径
set(CMAKE_C_COMPILER "/usr/bin/gcc")
set(CMAKE_CXX_COMPILER "/usr/bin/g++")

# 设置编译选项
set(CMAKE_C_FLAGS "-Wall")
set(CMAKE_CXX_FLAGS "-Wall")
