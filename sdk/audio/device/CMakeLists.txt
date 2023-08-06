cmake_minimum_required(VERSION 3.2)

project(AudioDevice)

find_package(SDL2 REQUIRED)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../common)

add_library(AudioDevice SHARED AudioDevice.cpp AudioDevice.h)

target_link_libraries(AudioDevice SDL2::SDL2)