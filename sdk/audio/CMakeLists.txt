cmake_minimum_required(VERSION 3.2)

project(Audio)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/device)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/resample)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/decode)