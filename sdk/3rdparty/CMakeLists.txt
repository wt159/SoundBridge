cmake_minimum_required(VERSION 3.2)

project(3rdparty)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/utils.cmake)

set(3RDPARTY_DIR ${CMAKE_CURRENT_LIST_DIR})
set(3RDPARTY_INSTALL_DIR ${3RDPARTY_DIR}/dist/${CMAKE_TARGET_3RDPARTY_NAME})
set(CMAKE_INSTALL_PREFIX ${3RDPARTY_INSTALL_DIR})
message(STATUS "3RDPARTY_DIR: ${3RDPARTY_DIR}")
message(STATUS "3RDPARTY_INSTALL_DIR: ${3RDPARTY_INSTALL_DIR}")
include(${CMAKE_CURRENT_LIST_DIR}/build_option.cmake)
# 判断目录是否存在
if(NOT EXISTS ${3RDPARTY_INSTALL_DIR})
    # 创建目录及其父级目录
    file(MAKE_DIRECTORY ${3RDPARTY_INSTALL_DIR} ${3RDPARTY_INSTALL_DIR}/../)
else()
    message(STATUS "3RDPARTY_INSTALL_DIR: ${3RDPARTY_INSTALL_DIR} already exists.")
    # return()
endif()

# boost
if(BUILD_OPTION_BOOST STREQUAL "ON")
    set(BOOST_ROOT ${3RDPARTY_DIR}/boost_1_81_0)
    if(CMAKE_TARGET_SYSTEM_NAME STREQUAL "windows")
        set(BOOST_BUILD_SCRIPT_DEP "${BOOST_ROOT}/windows_build.bat")
        message(STATUS "BOOST_BUILD_SCRIPT_DEP: ${BOOST_BUILD_SCRIPT_DEP}")
        execute_process(
            COMMAND ${BOOST_BUILD_SCRIPT_DEP} ${CMAKE_TARGET_SYSTEM_NAME} ${CMAKE_TARGET_SYSTEM_PROCESSOR} ${CMAKE_TARGET_TOOLCHAIN_NAME} ${INSTALL_DIR}
            COMMAND_ECHO STDOUT
        )
    else()
        set(BOOST_BUILD_SCRIPT_DEP "${BOOST_ROOT}/linux_build.sh")
        execute_process(
            COMMAND bash ${BOOST_BUILD_SCRIPT_DEP} ${CMAKE_TARGET_SYSTEM_NAME} ${CMAKE_TARGET_SYSTEM_PROCESSOR} ${CMAKE_TARGET_TOOLCHAIN_NAME}  ${INSTALL_DIR}
            COMMAND_ECHO STDOUT
        )
    endif()
endif()


# ffmpeg
if(BUILD_OPTION_FFMPEG STREQUAL "ON")
    # 执行bash命令检查是否安装
    execute_process(
        COMMAND bash -c "echo 'Bash is installed.'"
        RESULT_VARIABLE BASH_RESULT
        OUTPUT_QUIET
    )
    # 判断返回值
    if(BASH_RESULT EQUAL 0)
        message("Bash is installed.")
    else()
        # windows下没有bash，需要安装git软件, 然后把git安装目录下的bin目录添加到环境变量path中
        message(FATAL_ERROR "Bash is not installed. Please install bash and try again.")
    endif()
    set(FFMPEG_ROOT ${3RDPARTY_DIR}/ffmpeg-4.4.4)
    set(FFMPEG_BUILD_SCRIPT_DEP "${FFMPEG_ROOT}/build.sh")
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/ffmpeg_generated_files.txt
        COMMAND bash ${FFMPEG_BUILD_SCRIPT_DEP} ${CMAKE_TARGET_SYSTEM_NAME} ${CMAKE_TARGET_SYSTEM_PROCESSOR} ${CMAKE_TARGET_TOOLCHAIN_NAME} ${INSTALL_DIR}
        DEPENDS ${FFMPEG_BUILD_SCRIPT_DEP}
    )
    add_custom_target(
        generate_ffmpeg_files ALL
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/ffmpeg_generated_files.txt
    )
endif()

# flac
if(BUILD_OPTION_FLAC STREQUAL "ON")
    set(FLAC_ROOT ${3RDPARTY_DIR}/flac-1.4.3)
    add_subdirectory(${FLAC_ROOT})
endif()

# g3log
if(BUILD_OPTION_G3LOG STREQUAL "ON")
    add_subdirectory(${3RDPARTY_DIR}/g3log)
endif()
# zlib
if(BUILD_OPTION_ZLIB STREQUAL "ON")
    add_subdirectory(${3RDPARTY_DIR}/zlib)
endif()
# g3sinks
if(BUILD_OPTION_G3SINKS STREQUAL "ON")
    add_subdirectory(${3RDPARTY_DIR}/g3sinks)
endif()

# SDL
if(BUILD_OPTION_SDL2 STREQUAL "ON")
    add_subdirectory(${3RDPARTY_DIR}/SDL-release-2.28.1)
endif()

# ogg
if(BUILD_OPTION_OGG STREQUAL "ON")
    add_subdirectory(${3RDPARTY_DIR}/ogg-1.3.5)
endif()

# vorbis
if(BUILD_OPTION_VORBIS STREQUAL "ON")
    add_subdirectory(${3RDPARTY_DIR}/vorbis-1.3.7)
endif()
