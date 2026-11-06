cmake_minimum_required(VERSION 3.5)

project(sdk_tests)

# ============================================================
# Doctest Integration (Header-only, local copy)
# ============================================================
set(DOCTEST_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/doctest/doctest.h")
if(NOT EXISTS "${DOCTEST_HEADER}")
    message(FATAL_ERROR "doctest.h not found at ${DOCTEST_HEADER}")
endif()
message(STATUS "Using local doctest: ${DOCTEST_HEADER}")

# ============================================================
# Link Directories
# ============================================================
link_directories(${3RDPARTY_DIST_DIR}/lib)
link_directories(${3RDPARTY_DIST_DIR}/bin)

# ============================================================
# Shared Test Library Configuration
# ============================================================
set(SDK_TEST_COMMON_LIBS
    sdk
    LogWrapper
    Extractor
    AudioResample
    AudioDecode
    utils
)

set(SDK_TEST_COMMON_INCLUDES
    ${SDK_ROOT_DIR}
    ${SDK_ROOT_DIR}/extractor
    ${SDK_ROOT_DIR}/utils
    ${SDK_ROOT_DIR}/audio/common
    ${SDK_ROOT_DIR}/audio/resample
    ${SDK_ROOT_DIR}/audio/decode
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks
)

# ============================================================
# Legacy TestSdkSuite (deprecated - for backward compatibility)
# ============================================================
set(SDK_TEST_SOURCES
    TestSdkSuite.cpp
)

add_executable(TestSdkSuite ${SDK_TEST_SOURCES})

target_link_libraries(TestSdkSuite
    PRIVATE
    ${SDK_TEST_COMMON_LIBS}
)

target_include_directories(TestSdkSuite
    PRIVATE
    ${SDK_TEST_COMMON_INCLUDES}
)

set(SDK_TEST_PATH "${3RDPARTY_DIST_DIR}/bin;$ENV{PATH}")

if(WIN32 AND DEFINED SOUNDBRIDGE_QT_TEST_BIN_DIR AND NOT "${SOUNDBRIDGE_QT_TEST_BIN_DIR}" STREQUAL "")
    set(SDK_TEST_PATH "${3RDPARTY_DIST_DIR}/bin;${SOUNDBRIDGE_QT_TEST_BIN_DIR};$ENV{PATH}")
endif()

if(UNIX)
    set(SDK_TEST_LD_PATH "${3RDPARTY_DIST_DIR}/lib")
endif()

add_test(
    NAME sdk_core_tests
    COMMAND ${CMAKE_COMMAND} -E env "PATH=${SDK_TEST_PATH}" $<TARGET_FILE:TestSdkSuite> core
)
set_tests_properties(sdk_core_tests PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    ENVIRONMENT "LD_LIBRARY_PATH=${SDK_TEST_LD_PATH}")

add_test(
    NAME sdk_resample_tests
    COMMAND ${CMAKE_COMMAND} -E env "PATH=${SDK_TEST_PATH}" $<TARGET_FILE:TestSdkSuite> resample
)
set_tests_properties(sdk_resample_tests PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    ENVIRONMENT "LD_LIBRARY_PATH=${SDK_TEST_LD_PATH}")

add_test(
    NAME sdk_decode_tests
    COMMAND ${CMAKE_COMMAND} -E env "PATH=${SDK_TEST_PATH}" $<TARGET_FILE:TestSdkSuite> decode
)
set_tests_properties(sdk_decode_tests PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    ENVIRONMENT "LD_LIBRARY_PATH=${SDK_TEST_LD_PATH}")

add_test(
    NAME sdk_player_tests
    COMMAND ${CMAKE_COMMAND} -E env "PATH=${SDK_TEST_PATH}" $<TARGET_FILE:TestSdkSuite> player
)
set_tests_properties(sdk_player_tests PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    ENVIRONMENT "LD_LIBRARY_PATH=${SDK_TEST_LD_PATH}")

add_test(
    NAME sdk_all_tests
    COMMAND ${CMAKE_COMMAND} -E env "PATH=${SDK_TEST_PATH}" $<TARGET_FILE:TestSdkSuite> all
)
set_tests_properties(sdk_all_tests PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    ENVIRONMENT "LD_LIBRARY_PATH=${SDK_TEST_LD_PATH}")

set(SB_MEDIA_DIR "${SDK_ROOT_DIR}/../music")
add_test(
    NAME sdk_media_smoke
    COMMAND ${CMAKE_COMMAND} -E env "PATH=${SDK_TEST_PATH}" "SB_MEDIA_DIR=${SB_MEDIA_DIR}" $<TARGET_FILE:TestSdkSuite> media
)
set_tests_properties(sdk_media_smoke PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    ENVIRONMENT "LD_LIBRARY_PATH=${SDK_TEST_LD_PATH}")

add_test(
    NAME sdk_extractor_spec_tests
    COMMAND ${CMAKE_COMMAND} -E env "PATH=${SDK_TEST_PATH}" "SB_MEDIA_DIR=${SB_MEDIA_DIR}" $<TARGET_FILE:TestSdkSuite> extractor
)
set_tests_properties(sdk_extractor_spec_tests PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    ENVIRONMENT "LD_LIBRARY_PATH=${SDK_TEST_LD_PATH}")

# ============================================================
# New Unit Tests (Doctest-based)
# ============================================================
set(UNIT_TEST_SOURCES
    unit/UnitTestsMain.cpp
    unit/test_cosmos.cpp
    unit/test_utils.cpp
    unit/test_audio.cpp
    unit/test_audio_device.cpp
    unit/test_audio_device_lifecycle.cpp
    unit/test_audio_common.cpp
    unit/test_extractor.cpp
    unit/test_extractor_isolated.cpp
    unit/test_decode.cpp
    unit/test_stream_decoder.cpp
    unit/test_stream_decoder_contract.cpp
    unit/test_playback_position.cpp
    unit/test_playback_lifecycle.cpp
    unit/test_playlist.cpp
    unit/test_playlist_advance.cpp
    unit/test_player.cpp
    unit/test_player_integration.cpp
    unit/test_error.cpp
    unit/test_resample_formats.cpp
    unit/test_media_coverage.cpp
    unit/test_packet_factory.cpp
    unit/test_packet_supply.cpp
    unit/test_bad_input.cpp
    unit/test_streaming_budget.cpp
)

add_executable(UnitTests ${UNIT_TEST_SOURCES})

target_link_libraries(UnitTests
    PRIVATE
    ${SDK_TEST_COMMON_LIBS}
    AudioDevice
)

target_include_directories(UnitTests
    PRIVATE
    ${SDK_TEST_COMMON_INCLUDES}
    ${SDK_ROOT_DIR}/audio/device
    ${CMAKE_CURRENT_SOURCE_DIR}/unit
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
)

# 套件分组：单个 ctest 目标的墙钟时间必须给 TIMEOUT 留出真实余量，否则「挂死检测」
# 会退化成「正常跑完也超时」。2026-11-06 实测（Windows / mingw，全量 184 s）：
#   BadInput                        95 s / 55 例
#   StreamingBudget                 59 s / 24 例
#   其余全部套件合计                30 s / 413 例（其中 MusicPlayer Interactive Paths 2 s）
# 全量 184 s 挂在 TIMEOUT 180 上时，余量在 0~3 s 之间抖动，实测已出现 180.03 s 超时。
# 因此按耗时拆成四组：快集 / 坏输入 / 边解边播观测 / 真实音频交互。
# 分组只影响 ctest 的调度与超时口径，`ctest -R sdk_` 仍把四组全部跑完，
# `coverage` target 的覆盖面不变。
set(SB_TEST_BAD_INPUT_SUITES "BadInput")
set(SB_TEST_STREAMING_SUITES "StreamingBudget")
set(SB_TEST_INTERACTION_SUITES
    "MusicPlayer Interactive Paths,PublicPlayer Interactive Paths")

add_test(
    NAME sdk_unit_tests
    COMMAND ${CMAKE_COMMAND} -E env "PATH=${SDK_TEST_PATH}" $<TARGET_FILE:UnitTests>
            "-tse=${SB_TEST_BAD_INPUT_SUITES},${SB_TEST_STREAMING_SUITES},${SB_TEST_INTERACTION_SUITES}"
)
set_tests_properties(sdk_unit_tests PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    ENVIRONMENT "LD_LIBRARY_PATH=${SDK_TEST_LD_PATH}"
    # 快集实测约 30 s，180 s 留 6 倍余量。
    TIMEOUT 180)

add_test(
    NAME sdk_badinput_tests
    COMMAND ${CMAKE_COMMAND} -E env "PATH=${SDK_TEST_PATH}" $<TARGET_FILE:UnitTests>
            "-ts=${SB_TEST_BAD_INPUT_SUITES}"
)
set_tests_properties(sdk_badinput_tests PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    ENVIRONMENT "LD_LIBRARY_PATH=${SDK_TEST_LD_PATH}"
    # 坏输入用例里有「解析循环原地打转」这一类缺陷，一条挂死的用例会让 CI 一直等到
    # ctest 默认的 1500 秒（规格 10.#5）。实测 95 s，240 s 给 2.5 倍余量，
    # 仍能在几分钟内失败而不是等 25 分钟。
    TIMEOUT 240)

add_test(
    NAME sdk_streaming_tests
    COMMAND ${CMAKE_COMMAND} -E env "PATH=${SDK_TEST_PATH}" $<TARGET_FILE:UnitTests>
            "-ts=${SB_TEST_STREAMING_SUITES}"
)
set_tests_properties(sdk_streaming_tests PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    ENVIRONMENT "LD_LIBRARY_PATH=${SDK_TEST_LD_PATH}"
    # 实测 59 s，180 s 留 3 倍余量。
    TIMEOUT 180)

add_test(
    NAME sdk_interaction_tests
    COMMAND ${CMAKE_COMMAND} -E env "PATH=${SDK_TEST_PATH}" $<TARGET_FILE:UnitTests>
            "-ts=${SB_TEST_INTERACTION_SUITES}"
)
set_tests_properties(sdk_interaction_tests PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    ENVIRONMENT "LD_LIBRARY_PATH=${SDK_TEST_LD_PATH}"
    # 真实音频设备 + 真实文件，单独一组：设备打不开或播放卡住都不连累快集。
    # 现在 2 s（2 例），180 s 是给后续补交互用例留的空间（issue #72）。
    TIMEOUT 180)

# ============================================================
# Coverage Report Targets (Linux only, gated by root CMakeLists.txt)
# ============================================================
if(COVERAGE_ENABLED)
    add_custom_target(coverage
        COMMAND ${CMAKE_COMMAND} -E env "SB_BUILD_DIR=${CMAKE_BINARY_DIR}" "SB_COVERAGE_MIN_PERCENT=33" "${SDK_ROOT_DIR}/../scripts/coverage/run_linux.sh"
        DEPENDS TestSdkSuite UnitTests
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Generate SDK coverage report"
    )

    add_custom_target(coverage_clean
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/coverage
        COMMAND find ${CMAKE_BINARY_DIR} -name "*.gcda" -delete
        COMMAND find ${CMAKE_BINARY_DIR} -name "*.gcno" -delete
        COMMAND find ${CMAKE_BINARY_DIR} -name "*.info" -delete
        COMMENT "Clean coverage files"
    )
endif()
