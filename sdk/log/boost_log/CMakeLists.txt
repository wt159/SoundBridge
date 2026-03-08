cmake_minimum_required(VERSION 3.2)

project(LogWrapper)

find_package(Boost REQUIRED COMPONENTS log log_setup filesystem thread regex chrono atomic system)

add_library(LogWrapper LogWrapper.cpp LogWrapper.h)

target_link_libraries(LogWrapper
    Boost::log
    Boost::log_setup
    Boost::filesystem
    Boost::thread
    Boost::regex
    Boost::chrono
    Boost::atomic
    Boost::system
)

target_include_directories(LogWrapper PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
