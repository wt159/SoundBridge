cmake_minimum_required(VERSION 3.2)

project(3rdparty)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_VERBOSE_MAKEFILE ON)

set(3RDPARTY_DIR ${CMAKE_CURRENT_LIST_DIR})
set(3RDPARTY_INSTALL_DIR ${3RDPARTY_DIR}/dist/${CMAKE_TARGET_SYSTEM_NAME}_${CMAKE_TARGET_SYSTEM_PROCESSOR}_${CMAKE_TARGET_TOOLCHAIN_NAME})
message(STATUS "3RDPARTY_DIR: ${3RDPARTY_DIR}")
message(STATUS "3RDPARTY_INSTALL_DIR: ${3RDPARTY_INSTALL_DIR}")
# 判断目录是否存在
if(NOT EXISTS ${3RDPARTY_INSTALL_DIR})
    # 创建目录及其父级目录
    file(MAKE_DIRECTORY ${3RDPARTY_INSTALL_DIR} ${3RDPARTY_INSTALL_DIR}/../)
else()
    message(STATUS "3RDPARTY_INSTALL_DIR: ${3RDPARTY_INSTALL_DIR} already exists.")
    # return()
endif()

# boost
set(BOOST_ROOT ${3RDPARTY_DIR}/boost_1_81_0)
if(CMAKE_TARGET_SYSTEM_NAME STREQUAL "windows")
    set(BOOST_BUILD_SCRIPT_DEP "${BOOST_ROOT}/windows_build.bat")
    message(STATUS "BOOST_BUILD_SCRIPT_DEP: ${BOOST_BUILD_SCRIPT_DEP}")
    execute_process(
        COMMAND ${BOOST_BUILD_SCRIPT_DEP} ${CMAKE_TARGET_SYSTEM_NAME} ${CMAKE_TARGET_SYSTEM_PROCESSOR} ${CMAKE_TARGET_TOOLCHAIN_NAME}
        COMMAND_ECHO STDOUT
    )
else()
    set(BOOST_BUILD_SCRIPT_DEP "${BOOST_ROOT}/linux_build.sh")
    execute_process(
        COMMAND bash ${BOOST_BUILD_SCRIPT_DEP} ${CMAKE_TARGET_SYSTEM_NAME} ${CMAKE_TARGET_SYSTEM_PROCESSOR} ${CMAKE_TARGET_TOOLCHAIN_NAME}
        COMMAND_ECHO STDOUT
    )
endif()

# ffmpeg
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
    COMMAND bash ${FFMPEG_BUILD_SCRIPT_DEP} ${CMAKE_TARGET_SYSTEM_NAME} ${CMAKE_TARGET_SYSTEM_PROCESSOR} ${CMAKE_TARGET_TOOLCHAIN_NAME}
    DEPENDS ${FFMPEG_BUILD_SCRIPT_DEP}
)
add_custom_target(
    generate_ffmpeg_files ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/ffmpeg_generated_files.txt
)

# g3log
add_subdirectory(${3RDPARTY_DIR}/g3log)
# zlib
add_subdirectory(${3RDPARTY_DIR}/zlib)
# g3sinks
add_subdirectory(${3RDPARTY_DIR}/g3sinks)