cmake_minimum_required(VERSION 3.2)

project(sdk_tests)

link_directories(${3RDPARTY_DIST_DIR}/lib)
link_directories(${3RDPARTY_DIST_DIR}/bin)

set(SDK_TEST_SOURCES
    TestSdkSuite.cpp
)

add_executable(TestSdkSuite ${SDK_TEST_SOURCES})

target_link_libraries(TestSdkSuite
    PRIVATE
    LogWrapper
    Extractor
    AudioResample
    AudioDecode
    utils
)

target_include_directories(TestSdkSuite
    PRIVATE
    ${SDK_ROOT_DIR}
    ${SDK_ROOT_DIR}/extractor
    ${SDK_ROOT_DIR}/utils
    ${SDK_ROOT_DIR}/audio/common
    ${SDK_ROOT_DIR}/audio/resample
    ${SDK_ROOT_DIR}/audio/decode
)

set(SDK_TEST_PATH "${3RDPARTY_DIST_DIR}/bin;$ENV{PATH}")

if(WIN32 AND DEFINED SOUNDBRIDGE_QT_TEST_BIN_DIR AND NOT "${SOUNDBRIDGE_QT_TEST_BIN_DIR}" STREQUAL "")
    set(SDK_TEST_PATH "${3RDPARTY_DIST_DIR}/bin;${SOUNDBRIDGE_QT_TEST_BIN_DIR};$ENV{PATH}")
endif()

add_test(
    NAME sdk_core_tests
    COMMAND ${CMAKE_COMMAND} -E env "PATH=${SDK_TEST_PATH}" $<TARGET_FILE:TestSdkSuite> core
)
set_tests_properties(sdk_core_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

add_test(
    NAME sdk_resample_tests
    COMMAND ${CMAKE_COMMAND} -E env "PATH=${SDK_TEST_PATH}" $<TARGET_FILE:TestSdkSuite> resample
)
set_tests_properties(sdk_resample_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

add_test(
    NAME sdk_decode_tests
    COMMAND ${CMAKE_COMMAND} -E env "PATH=${SDK_TEST_PATH}" $<TARGET_FILE:TestSdkSuite> decode
)
set_tests_properties(sdk_decode_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

add_test(
    NAME sdk_all_tests
    COMMAND ${CMAKE_COMMAND} -E env "PATH=${SDK_TEST_PATH}" $<TARGET_FILE:TestSdkSuite> all
)
set_tests_properties(sdk_all_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

set(SB_MEDIA_DIR "${SDK_ROOT_DIR}/../music")
add_test(
    NAME sdk_media_smoke
    COMMAND ${CMAKE_COMMAND} -E env "PATH=${SDK_TEST_PATH}" "SB_MEDIA_DIR=${SB_MEDIA_DIR}" $<TARGET_FILE:TestSdkSuite> media
)
set_tests_properties(sdk_media_smoke PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
