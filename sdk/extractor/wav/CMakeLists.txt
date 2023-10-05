cmake_minimum_required(VERSION 3.2)

project(WavExtractor)

include_directories(${3RDPARTY_DIST_DIR}/include)

# find_package(Boost_filesystem REQUIRED)

set(SOURCES "WAVExtractor.cpp")

add_library(WavExtractor ${SOURCES})

# target_link_libraries(WavExtractor Boost::filesystem LogWrapper)