cmake_minimum_required(VERSION 3.2)

project(test)

include_directories(${3RDPARTY_DIST_DIR}/include)
link_directories(${3RDPARTY_DIST_DIR}/bin)

add_executable(TestLogWrapper TestLogWrapper.cpp)
target_link_libraries(TestLogWrapper LogWrapper)

add_executable(TestAudioDevice TestAudioDevice.cpp)
target_link_libraries(TestAudioDevice AudioDevice)

add_executable(TestAudioResample TestAudioResample.cpp)
target_link_libraries(TestAudioResample AudioResample)

add_executable(TestAudioDecode TestAudioDecode.cpp)
target_link_libraries(TestAudioDecode AudioDecode)

add_executable(TestMusicPlayer TestMusicPlayer.cpp)
target_link_libraries(TestMusicPlayer sdk)

add_executable(TestFFmpegAudioDecode TestFFmpegAudioDecode.cpp)
target_link_libraries(TestAudioDecode avformat avcodec avutil swresample)
