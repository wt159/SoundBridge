# toolchain.windows_mingw.cmake
# 1. mkdir build && cd build
# 2. cmake .. --no-warn-unused-cli -Wno-dev -DCMAKE_BUILD_TYPE:STRING=Debug -DCMAKE_TOOLCHAIN_FILE=%cd%\..\toolchain.windows_x86_64_mingw.cmake -G "MinGW Makefiles"
set(CMAKE_TARGET_SYSTEM_NAME windows)
set(CMAKE_TARGET_SYSTEM_PROCESSOR x86_64)
set(CMAKE_TARGET_TOOLCHAIN_NAME gcc)
set(CMAKE_TARGET_BUILD_TYPE Debug)
string(TOLOWER "${CMAKE_BUILD_TYPE}" CMAKE_TARGET_BUILD_TYPE)

# 设置C和C++编译器路径
set(QT_ROOT_PATH "F:/Qt/Qt5.14.2" CACHE PATH "Qt root path")
set(QT_VERSION "5.14.2" CACHE STRING "Qt version")
set(QT_MINGW_DIR "mingw73_64" CACHE STRING "Qt MinGW target directory")
set(QT_TOOLS_MINGW_DIR "mingw730_64" CACHE STRING "Qt Tools MinGW directory")
set(QT_MINGW_PATH "${QT_ROOT_PATH}/${QT_VERSION}/${QT_MINGW_DIR}" CACHE PATH "Qt mingw path")
set(QT_LIB_PATH "${QT_MINGW_PATH}/lib" CACHE PATH "Qt mingw lib path")
set(QT_TOOLS_BIN_PATH "${QT_ROOT_PATH}/Tools/${QT_TOOLS_MINGW_DIR}/bin" CACHE PATH "Qt tools bin path")
set(CMAKE_C_COMPILER "${QT_TOOLS_BIN_PATH}/gcc.exe")
set(CMAKE_CXX_COMPILER "${QT_TOOLS_BIN_PATH}/g++.exe")

# 设置编译选项
set(CMAKE_C_FLAGS "-Wall")
set(CMAKE_CXX_FLAGS "-Wall")

set(BUILD_OPTION_G3SINKS ON)
set(BUILD_OPTION_BOOST ON)
set(BUILD_OPTION_FFMPEG ON)
