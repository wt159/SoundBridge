cmake_minimum_required(VERSION 3.2)

project(Extractor)

include_directories(${3RDPARTY_DIST_DIR}/include)
include_directories(${3RDPARTY_DIST_DIR}/include/boost-1_81)
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../audio/common)

find_package(Boost_filesystem REQUIRED)

file(GLOB_RECURSE SOURCES "*.cpp")
# message(STATUS "Extractor sources: ${SOURCES}")
add_library(Extractor ${SOURCES})

target_link_libraries(Extractor utils Boost::filesystem LogWrapper AudioDecode)

target_include_directories(Extractor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../audio/common)