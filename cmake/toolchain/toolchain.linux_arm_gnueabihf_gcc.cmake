# toolchain.windows_mingw.cmake
# 1. mkdir build && cd build
# 2. cmake .. --no-warn-unused-cli -DCMAKE_BUILD_TYPE:STRING=Debug -DCMAKE_TOOLCHAIN_FILE=%cd%\..\toolchain.linux_x86_64_gcc.cmake
set(CMAKE_TARGET_SYSTEM_NAME linux)
set(CMAKE_TARGET_SYSTEM_PROCESSOR arm_gnueabihf)
set(CMAKE_TARGET_TOOLCHAIN_NAME gcc)
set(CMAKE_TARGET_BUILD_TYPE debug)
string(TOLOWER "${CMAKE_BUILD_TYPE}" CMAKE_TARGET_BUILD_TYPE)
set(CMAKE_TARGET_3RDPARTY_NAME ${CMAKE_TARGET_SYSTEM_NAME}_${CMAKE_TARGET_SYSTEM_PROCESSOR}_${CMAKE_TARGET_TOOLCHAIN_NAME}_${CMAKE_TARGET_BUILD_TYPE})
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 设置C和C++编译器路径
set(CMAKE_C_COMPILER "/usr/local/arm/gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabihf/bin/arm-linux-gnueabihf-gcc")
set(CMAKE_CXX_COMPILER "/usr/local/arm/gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabihf/bin/arm-linux-gnueabihf-g++")

# 设置编译选项
set(CMAKE_C_FLAGS "-Werror ")
set(CMAKE_CXX_FLAGS "-Werror")
# 指定alsa-lib动态库路径
list(APPEND CMAKE_PREFIX_PATH "/home/cosmos/workspace/imx6ull_alientek/nfs/rootfs/usr")
list(APPEND CMAKE_PREFIX_PATH "/home/cosmos/workspace/imx6ull_alientek/nfs/rootfs/usr/qt5")