cmake_minimum_required(VERSION 3.2)

project(AacExtractor)

include_directories(${3RDPARTY_DIST_DIR}/include)
include_directories(${CMAKE_CURRENT_LIST_DIR}/../../)

set(SOURCES "AACExtractor.cpp")

add_library(AacExtractor ${SOURCES})

target_link_libraries(AacExtractor
    AudioDecode)
