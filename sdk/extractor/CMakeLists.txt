cmake_minimum_required(VERSION 3.2)

project(Extractor)

include_directories(${3RDPARTY_DIST_DIR}/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

find_package(Boost_filesystem REQUIRED)

file(GLOB_RECURSE SOURCES "*.cpp")

add_library(Extractor ${SOURCES})

target_link_libraries(Extractor Boost::filesystem LogWrapper)

add_subdirectory(wav)