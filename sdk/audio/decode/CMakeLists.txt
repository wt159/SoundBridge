cmake_minimum_required(VERSION 3.2)

project(AudioDecode)

set(EXT_3RDPARTY_DIST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../audio_video_study_on_ffmpeg_sdl/dist/win/ffmpeg-4.3.2-2021-02-27-full_build-shared/)
message(STATUS "3RDPARTY_DIST_DIR: ${3RDPARTY_DIST_DIR}")
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../common)
include_directories(${EXT_3RDPARTY_DIST_DIR}/include)
link_directories(${EXT_3RDPARTY_DIST_DIR}/lib)
link_directories(${EXT_3RDPARTY_DIST_DIR}/bin)

add_library(AudioDecode SHARED AudioDecode.cpp AudioDecode.h)

add_executable(AudioDecodeTest test.cpp)

target_link_libraries(AudioDecode avcodec avutil avformat LogWrapper)
target_link_libraries(AudioDecodeTest LogWrapper AudioDecode)