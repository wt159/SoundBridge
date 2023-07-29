cmake_minimum_required(VERSION 3.2)

project(AudioResample)


include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../common)
include_directories(${3RDPARTY_DIST_DIR}/include)
link_directories(${3RDPARTY_DIST_DIR}/lib)
link_directories(${3RDPARTY_DIST_DIR}/bin)

add_library(AudioResample SHARED AudioResample.cpp AudioResample.h)

add_executable(AudioResampleTest test.cpp)

target_link_libraries(AudioResample swresample avutil LogWrapper)
target_link_libraries(AudioResampleTest -static LogWrapper AudioResample)