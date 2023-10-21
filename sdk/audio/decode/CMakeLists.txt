cmake_minimum_required(VERSION 3.2)

project(AudioDecode)

include_directories(${SDK_ROOT_DIR}/extractor)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../common)
include_directories(${3RDPARTY_DIST_DIR}/include)
link_directories(${3RDPARTY_DIST_DIR}/lib)
link_directories(${3RDPARTY_DIST_DIR}/bin)

add_library(AudioDecode AudioDecode.cpp AudioDecodeProcess.cpp)

target_link_libraries(AudioDecode avcodec avutil avformat LogWrapper utils)
target_include_directories(AudioDecode
                            PUBLIC
                            ${CMAKE_CURRENT_SOURCE_DIR}
                            ${3RDPARTY_DIST_DIR}/include
                            ${CMAKE_CURRENT_SOURCE_DIR}/../common
)