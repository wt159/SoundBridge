cmake_minimum_required(VERSION 3.2)

project(test)


include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../audio/common)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../audio/device)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../audio/resample)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../audio/decode)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../file)
link_directories(${3RDPARTY_DIST_DIR}/bin)

add_executable(TestLogWrapper TestLogWrapper.cpp)
target_link_libraries(TestLogWrapper LogWrapper)

add_executable(TestAudioDevice TestAudioDevice.cpp)
target_link_libraries(TestAudioDevice LogWrapper AudioDevice)

add_executable(TestAudioResample TestAudioResample.cpp)
target_link_libraries(TestAudioResample LogWrapper AudioResample)

add_executable(TestAudioDecode TestAudioDecode.cpp)
target_link_libraries(TestAudioDecode LogWrapper AudioDecode)

# add_executable(TestFileSearch TestFileSearch.cpp)
# target_link_libraries(TestFileSearch FileSearch)