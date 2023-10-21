cmake_minimum_required(VERSION 3.2)

project(utils)

add_library(utils ByteUtils.cpp AudioBuffer.cpp)

target_include_directories(utils PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})