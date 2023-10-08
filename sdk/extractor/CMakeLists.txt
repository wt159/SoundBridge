cmake_minimum_required(VERSION 3.2)

project(Extractor)

include_directories(${3RDPARTY_DIST_DIR}/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/wav)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/aac)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/mp3)

find_package(Boost_filesystem REQUIRED)

file(GLOB_RECURSE SOURCES "*.cpp")
message(STATUS "Extractor sources: ${SOURCES}")
add_library(Extractor ${SOURCES})

target_link_libraries(Extractor Boost::filesystem LogWrapper)
