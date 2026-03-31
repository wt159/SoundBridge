cmake_minimum_required(VERSION 3.2)

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
# Test Framework Options
# ============================================================
option(ENABLE_COVERAGE "Enable code coverage reporting (Linux/macOS only)" OFF)
option(COVERAGE_EXCLUDE_3RDPARTY "Exclude 3rdparty from coverage" ON)

# ============================================================
# Coverage Configuration (Linux/macOS only)
# ============================================================
if(ENABLE_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        if(UNIX OR APPLE)
            message(STATUS "Coverage enabled (${CMAKE_SYSTEM_NAME})")
            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} --coverage -fprofile-arcs -ftest-coverage")
            set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --coverage")
            set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} --coverage")
        else()
            message(WARNING "Coverage not supported on ${CMAKE_SYSTEM_NAME} - disabling")
            set(ENABLE_COVERAGE OFF)
        endif()
    else()
        message(WARNING "Coverage not supported for ${CMAKE_CXX_COMPILER_ID}")
        set(ENABLE_COVERAGE OFF)
    endif()
endif()

# Coverage exclude patterns
if(ENABLE_COVERAGE AND COVERAGE_EXCLUDE_3RDPARTY)
    set(COVERAGE_EXCLUDE_PATTERNS
        "*/3rdparty/*"
        "*/test/*"
        "*/build/*"
    )
endif()

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
    unit/test_audio_common.cpp
    unit/test_extractor.cpp
    unit/test_extractor_isolated.cpp
    unit/test_decode.cpp
    unit/test_playlist.cpp
    unit/test_player.cpp
    unit/test_media_smoke.cpp
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

add_test(
    NAME sdk_unit_tests
    COMMAND ${CMAKE_COMMAND} -E env "PATH=${SDK_TEST_PATH}" $<TARGET_FILE:UnitTests>
)
set_tests_properties(sdk_unit_tests PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    ENVIRONMENT "LD_LIBRARY_PATH=${SDK_TEST_LD_PATH}")

# ============================================================
# Coverage Report Target (Linux/macOS only)
# ============================================================
if(ENABLE_COVERAGE)
    add_custom_target(coverage
        COMMAND ${CMAKE_COMMAND} -E echo "Running tests with coverage..."
        COMMAND ctest -R sdk_ --output-on-failure
        COMMAND ${CMAKE_COMMAND} -E echo "Generating coverage report..."
        COMMAND lcov --directory ${CMAKE_BINARY_DIR} 
            --capture 
            --output-file coverage.info
            --binary ${CMAKE_BINARY_DIR}/sdk/test/UnitTests
        COMMAND lcov --remove coverage.info 
            ${COVERAGE_EXCLUDE_PATTERNS}
            --output-file coverage.filtered.info
        COMMAND genhtml coverage.filtered.info 
            --output-directory ${CMAKE_BINARY_DIR}/coverage_html
            --title "SoundBridge SDK Coverage"
            --show-details
            --legend
        COMMAND ${CMAKE_COMMAND} -E echo "Coverage report: file://${CMAKE_BINARY_DIR}/coverage_html/index.html"
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Generate code coverage report"
        DEPENDS UnitTests
    )
    
    add_custom_target(coverage_clean
        COMMAND find ${CMAKE_BINARY_DIR} -name "*.gcno" -o -name "*.gcda" -o -name "*.info" | xargs rm -f
        COMMAND rm -rf ${CMAKE_BINARY_DIR}/coverage_html
        COMMENT "Clean coverage files"
    )
endif()
