cmake_minimum_required(VERSION 3.2)

project(FileSearch)

include_directories(${3RDPARTY_DIST_DIR}/include)

find_package(Boost_filesystem REQUIRED)

add_library(FileSearch SHARED FileSearch.cpp FileSearch.h)

target_link_libraries(FileSearch Boost::filesystem LogWrapper)