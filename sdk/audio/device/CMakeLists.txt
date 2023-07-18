cmake_minimum_required(VERSION 3.2)

project(AudioDevice)

# 查找g3log和g3sinks库
# find_package(Boost_filesystem REQUIRED)
# find_package(g3log REQUIRED)
# find_package(g3sinks REQUIRED)

# 添加LogWrapper库
add_library(${CMAKE_PROJECT_NAME} SHARED AudioDevice.cpp AudioDevice.h)

# 添加可执行文件test
# add_executable(${CMAKE_PROJECT_NAME}Test test.cpp)

# 链接g3log和g3sinks库
# target_include_directories(${CMAKE_PROJECT_NAME} PUBLIC ${G3LOG_INCLUDE_DIRS})
# target_include_directories(${CMAKE_PROJECT_NAME}Test PUBLIC ${G3LOG_INCLUDE_DIRS})
# target_link_libraries(${CMAKE_PROJECT_NAME} ${G3LOG_LIBRARY} G3::g3logrotate)
# target_link_libraries(${CMAKE_PROJECT_NAME}Test -static LogWrapper)