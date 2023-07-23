cmake_minimum_required(VERSION 3.2)

project(AudioDevice)

# 查找g3log和g3sinks库
find_package(SDL2 REQUIRED)

# 头文件
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../common)

# 添加LogWrapper库
add_library(AudioDevice SHARED AudioDevice.cpp AudioDevice.h)

# 添加可执行文件test
add_executable(AudioDeviceTest test.cpp)

# 链接g3log和g3sinks库
# target_include_directories(${CMAKE_PROJECT_NAME} PUBLIC ${G3LOG_INCLUDE_DIRS})
# target_include_directories(${CMAKE_PROJECT_NAME}Test PUBLIC ${G3LOG_INCLUDE_DIRS})
target_link_libraries(AudioDevice SDL2::SDL2)
target_link_libraries(AudioDeviceTest -static LogWrapper AudioDevice)