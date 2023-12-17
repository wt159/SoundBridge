cmake_minimum_required(VERSION 3.2)

project(LogWrapper)

find_package(Boost_filesystem REQUIRED)
find_package(Boost_log REQUIRED)
find_package(Boost_thread REQUIRED)

add_library(LogWrapper LogWrapper.cpp LogWrapper.h)

target_link_libraries(LogWrapper Boost::log)

target_include_directories(LogWrapper PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})