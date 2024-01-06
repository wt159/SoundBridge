cmake_minimum_required(VERSION 3.2)

project(AudioDecode)

include_directories(${SDK_ROOT_DIR}/extractor)
include_directories(${SDK_ROOT_DIR}/utils)
# include_directories(${SDK_ROOT_DIR}/log)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../common)
include_directories(${3RDPARTY_DIST_DIR}/include)
link_directories(${3RDPARTY_DIST_DIR}/lib)
link_directories(${3RDPARTY_DIST_DIR}/bin)

find_package(Vorbis REQUIRED)
find_package(Ogg REQUIRED)

set(DECODE_SOURCE_FILES
    AudioDecode.cpp
    FLACDecode.cpp
    VorbisDecode.cpp
    AudioDecodeProcess.cpp
    )

add_library(AudioDecode ${DECODE_SOURCE_FILES})

set(DECODE_LIBS
    avcodec
    avutil
    avformat
    FLAC++
    FLAC
    Vorbis::vorbis
    Ogg::ogg
    LogWrapper
    )

target_link_libraries(AudioDecode PRIVATE ${DECODE_LIBS})
target_include_directories(AudioDecode
                            PUBLIC
                            ${CMAKE_CURRENT_SOURCE_DIR}
                            ${3RDPARTY_DIST_DIR}/include
                            ${CMAKE_CURRENT_SOURCE_DIR}/../common
)