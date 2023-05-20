cmake_minimum_required(VERSION 3.2)

project(LogWrapper)

# 查找g3log和g3sinks库
find_package(Boost_filesystem REQUIRED)
find_package(g3log REQUIRED)
find_package(g3sinks REQUIRED)

# 添加LogWrapper库
add_library(LogWrapper SHARED LogWrapper.cpp LogWrapper.h)

# 添加可执行文件test
add_executable(LogWrapperTest test.cpp)

# 链接g3log和g3sinks库
target_include_directories(LogWrapper PUBLIC ${G3LOG_INCLUDE_DIRS})
target_include_directories(LogWrapperTest PUBLIC ${G3LOG_INCLUDE_DIRS})
target_link_libraries(LogWrapper ${G3LOG_LIBRARY} G3::g3logrotate)
target_link_libraries(LogWrapperTest -static LogWrapper)