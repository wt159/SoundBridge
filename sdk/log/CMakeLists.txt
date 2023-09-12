cmake_minimum_required(VERSION 3.2)

project(LogWrapper)


find_package(Boost_filesystem REQUIRED)
find_package(G3LOG REQUIRED)
find_package(g3sinks REQUIRED)

add_library(LogWrapper LogWrapper.cpp LogWrapper.h)

target_link_libraries(LogWrapper G3::g3logrotate)