cmake_minimum_required(VERSION 3.2)

project(LogWrapper)


find_package(Boost_filesystem REQUIRED)
find_package(g3log REQUIRED)
find_package(zlib REQUIRED)
find_package(g3sinks REQUIRED)

add_library(LogWrapper LogWrapper.cpp LogWrapper.h)

target_link_libraries(LogWrapper G3::g3logrotate zlib::zlib)

target_include_directories(LogWrapper PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})