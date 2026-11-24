cmake_minimum_required(VERSION 3.5)

project(SoundBridge_PROJECT)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/utils.cmake)
include(CTest)

option(ENABLE_COVERAGE "Enable code coverage reporting (Linux only)" OFF)

if(ENABLE_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(STATUS "Coverage enabled (${CMAKE_SYSTEM_NAME})")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} --coverage -fprofile-arcs -ftest-coverage")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} --coverage -fprofile-arcs -ftest-coverage")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --coverage")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} --coverage")
    set(COVERAGE_ENABLED TRUE)
endif()

# AddressSanitizer 取证通道（票 #298）：默认关，生产构建与 CI 零影响。
# 只做插桩与链接，不改任何目标定义；用法见 docs/research/asan-core-channel.md。
option(ENABLE_ASAN "Enable AddressSanitizer instrumentation (GCC/Clang, out-of-CI only)" OFF)

if(ENABLE_ASAN)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "ENABLE_ASAN requires GCC or Clang (current: ${CMAKE_CXX_COMPILER_ID})")
    endif()
    message(STATUS "AddressSanitizer enabled (${CMAKE_CXX_COMPILER_ID})")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=address")
endif()

set(ROOT_DIR ${CMAKE_CURRENT_LIST_DIR})
message(STATUS "CMAKE_TOOLCHAIN_FILE: ${CMAKE_TOOLCHAIN_FILE}")
set(SDK_DIR ${CMAKE_CURRENT_LIST_DIR}/sdk)
set(3RDPARTY_DIST_DIR ${SDK_DIR}/3rdparty/dist/${CMAKE_TARGET_3RDPARTY_NAME})

if(IS_DIRECTORY ${3RDPARTY_DIST_DIR})
    message(STATUS "${3RDPARTY_DIST_DIR} exist")
else()
    message(FATAL_ERROR "${3RDPARTY_DIST_DIR} not exist, please build 3rdparty")
endif()

add_subdirectory(${SDK_DIR})

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/app)

if(WIN32)
    set(SOUNDBRIDGE_QT_TEST_BIN_DIR
        "${QT_TOOLS_BIN_PATH}"
        CACHE PATH "Qt bin directory used when running Windows SDK tests"
    )

    set(SOUNDBRIDGE_PACKAGE_DIR
        "${CMAKE_CURRENT_LIST_DIR}/package/SoundBridge_portable_v3"
        CACHE PATH "Portable package output directory"
    )
    if(DEFINED QT_MINGW_PATH)
        set(_qt_bin_hint "${QT_MINGW_PATH}/bin")
    endif()
    find_program(SOUNDBRIDGE_WINDEPLOYQT
        NAMES windeployqt
        HINTS "${_qt_bin_hint}" "${QT_LIB_PATH}/../bin"
    )
    set(SOUNDBRIDGE_WINDEPLOYQT
        "${SOUNDBRIDGE_WINDEPLOYQT}"
        CACHE FILEPATH "Path to windeployqt"
    )

    add_custom_target(package_portable
        COMMAND ${CMAKE_COMMAND}
            -DAPP_EXE="$<TARGET_FILE:SoundBridge>"
            -DPACKAGE_DIR="${SOUNDBRIDGE_PACKAGE_DIR}"
            -DWINDEPLOYQT_EXECUTABLE="${SOUNDBRIDGE_WINDEPLOYQT}"
            -DTHIRD_PARTY_DLL_DIR="${3RDPARTY_DIST_DIR}/bin"
            -DMUSIC_DIR="${CMAKE_CURRENT_LIST_DIR}/music"
            -P "${CMAKE_CURRENT_LIST_DIR}/cmake/package_portable.cmake"
        DEPENDS SoundBridge
        COMMENT "Create portable Windows package"
    )
endif()

if(UNIX AND NOT APPLE)
    set(SOUNDBRIDGE_PACKAGE_DIR
        "${CMAKE_CURRENT_LIST_DIR}/package/SoundBridge_portable"
        CACHE PATH "Portable package output directory"
    )

    add_custom_target(package_portable
        COMMAND ${CMAKE_COMMAND}
            -DAPP_EXE="$<TARGET_FILE:SoundBridge>"
            -DPACKAGE_DIR="${SOUNDBRIDGE_PACKAGE_DIR}"
            -DTHIRD_PARTY_LIB_DIR="${3RDPARTY_DIST_DIR}/lib"
            -DMUSIC_DIR="${CMAKE_CURRENT_LIST_DIR}/music"
            -P "${CMAKE_CURRENT_LIST_DIR}/cmake/package_portable_linux.cmake"
        DEPENDS SoundBridge
        COMMENT "Create portable Linux package"
    )
endif()
