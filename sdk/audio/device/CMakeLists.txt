cmake_minimum_required(VERSION 3.2)

project(AudioDevice)

find_package(SDL2 REQUIRED)

include_directories(${SDK_ROOT_DIR}/log)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../common)

add_library(AudioDevice AudioDevice.cpp AudioDevice.h)

target_link_libraries(AudioDevice SDL2::SDL2)
target_include_directories(AudioDevice PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../common)