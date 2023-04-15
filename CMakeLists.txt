cmake_minimum_required(VERSION 3.2)

project(SoundBridge_MusicPlayer)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_VERBOSE_MAKEFILE ON)

include(${CMAKE_CURRENT_LIST_DIR}/Option.cmake)

set(SDK_DIR ${CMAKE_SOURCE_DIR}/sdk)
add_subdirectory(${SDK_DIR})