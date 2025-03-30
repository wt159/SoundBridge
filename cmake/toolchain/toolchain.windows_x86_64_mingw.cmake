# toolchain.windows_mingw.cmake
# 1. mkdir build && cd build
# 2. cmake .. --no-warn-unused-cli -Wno-dev -DCMAKE_BUILD_TYPE:STRING=Debug -DCMAKE_TOOLCHAIN_FILE=%cd%\..\toolchain.windows_x86_64_mingw.cmake -G "MinGW Makefiles"
set(CMAKE_TARGET_SYSTEM_NAME windows)
set(CMAKE_TARGET_SYSTEM_PROCESSOR x86_64)
set(CMAKE_TARGET_TOOLCHAIN_NAME gcc)
set(CMAKE_TARGET_BUILD_TYPE Debug)
string(TOLOWER "${CMAKE_BUILD_TYPE}" CMAKE_TARGET_BUILD_TYPE)

# 设置C和C++编译器路径
set(QT_ROOT_PATH "F:/Qt/Qt5.14.2")
set(QT_MINGW_PATH "${QT_ROOT_PATH}/5.14.2/mingw73_64" CACHE PATH "Qt mingw path")
set(QT_LIB_PATH "${QT_MINGW_PATH}/lib" CACHE PATH "Qt mingw lib path")
set(CMAKE_C_COMPILER "${QT_ROOT_PATH}/Tools/mingw730_64/bin/gcc.exe")
set(CMAKE_CXX_COMPILER "${QT_ROOT_PATH}/Tools/mingw730_64/bin/g++.exe")

# 设置编译选项
set(CMAKE_C_FLAGS "-Wall")
set(CMAKE_CXX_FLAGS "-Wall")

set(BUILD_OPTION_G3SINKS ON)
set(BUILD_OPTION_BOOST ON)
set(BUILD_OPTION_FFMPEG ON)