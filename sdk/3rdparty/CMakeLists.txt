cmake_minimum_required(VERSION 3.2)

project(3rdparty)

set(3RDPARTY_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(3RDPARTY_INSTALL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/dist/windows_amd64_gcc)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_VERBOSE_MAKEFILE ON)

include(${CMAKE_CURRENT_LIST_DIR}/../../Option.cmake)
message(STATUS "3RDPARTY_DIR: ${3RDPARTY_DIR}")

# boost
if(CMAKE_TARGET_SYSTEM_NAME STREQUAL "windows")
    set(BOOST_BUILD_SCRIPT_DEP "${3RDPARTY_DIR}/boost_1_81_0/windows_build.bat")
    set(BOOST_BUILD_SCRIPT "${BOOST_BUILD_SCRIPT_DEP}")
else()
    set(BOOST_BUILD_SCRIPT_DEP "${3RDPARTY_DIR}/boost_1_81_0/linux_build.sh")
    set(BOOST_BUILD_SCRIPT "sh ${BOOST_BUILD_SCRIPT_DEP}")
endif()
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/boost_generated_files.txt
    COMMAND ${BOOST_BUILD_SCRIPT} ${CMAKE_TARGET_SYSTEM_NAME} ${CMAKE_TARGET_SYSTEM_PROCESSOR} ${CMAKE_TARGET_TOOLCHAIN_NAME}
    DEPENDS ${BOOST_BUILD_SCRIPT_DEP}
)
add_custom_target(
    generate_boost_files ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/boost_generated_files.txt
)

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

add_subdirectory(${3RDPARTY_DIR}/g3log)

add_subdirectory(${3RDPARTY_DIR}/zlib)

add_subdirectory(${3RDPARTY_DIR}/g3sinks)

#ffmpeg
# execute_process(COMMAND start my_script.bat arg1 arg2)