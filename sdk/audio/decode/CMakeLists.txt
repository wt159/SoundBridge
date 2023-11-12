cmake_minimum_required(VERSION 3.2)

project(AudioDecode)

include_directories(${SDK_ROOT_DIR}/extractor)
include_directories(${SDK_ROOT_DIR}/utils)
include_directories(${SDK_ROOT_DIR}/log)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../common)
include_directories(${3RDPARTY_DIST_DIR}/include)
link_directories(${3RDPARTY_DIST_DIR}/lib)
link_directories(${3RDPARTY_DIST_DIR}/bin)

add_library(AudioDecode AudioDecode.cpp FLACDecode.cpp AudioDecodeProcess.cpp)

target_link_libraries(AudioDecode PRIVATE avcodec avutil avformat FLAC++ FLAC)
target_include_directories(AudioDecode
                            PUBLIC
                            ${CMAKE_CURRENT_SOURCE_DIR}
                            ${3RDPARTY_DIST_DIR}/include
                            ${CMAKE_CURRENT_SOURCE_DIR}/../common
)