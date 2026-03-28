cmake_minimum_required(VERSION 3.2)

project(SoundBridge_PROJECT)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/utils.cmake)
include(CTest)

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
