# package_portable_linux.cmake
# 打包 SoundBridge Linux 便携版

if(NOT UNIX OR APPLE)
    message(FATAL_ERROR "package_portable_linux is supported only on Linux")
endif()

if(NOT DEFINED APP_EXE)
    message(FATAL_ERROR "APP_EXE not set")
endif()
if(NOT EXISTS "${APP_EXE}")
    message(FATAL_ERROR "APP_EXE not found: ${APP_EXE}")
endif()

if(NOT DEFINED PACKAGE_DIR)
    message(FATAL_ERROR "PACKAGE_DIR not set")
endif()

if(NOT DEFINED THIRD_PARTY_LIB_DIR)
    message(FATAL_ERROR "THIRD_PARTY_LIB_DIR not set")
endif()

message(STATUS "Packaging to: ${PACKAGE_DIR}")
file(REMOVE_RECURSE "${PACKAGE_DIR}")
file(MAKE_DIRECTORY "${PACKAGE_DIR}")
file(MAKE_DIRECTORY "${PACKAGE_DIR}/lib")

# 复制可执行文件
file(COPY "${APP_EXE}" DESTINATION "${PACKAGE_DIR}")
file(CHMOD "${PACKAGE_DIR}/SoundBridge" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
     GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

# 使用 ldd 找到所有依赖的 .so 文件并复制
execute_process(
    COMMAND ldd "${APP_EXE}"
    OUTPUT_VARIABLE _ldd_output
    RESULT_VARIABLE _ldd_rv
)

if(NOT _ldd_rv EQUAL 0)
    message(FATAL_ERROR "ldd failed with code: ${_ldd_rv}")
endif()

# 解析 ldd 输出，提取库路径
string(REPLACE "\n" ";" _ldd_lines "${_ldd_output}")
foreach(_line ${_ldd_lines})
    # 匹配 "=> /path/to/lib.so" 模式
    if(_line MATCHES "=> ([^ ]+) \\(")
        set(_lib_path "${CMAKE_MATCH_1}")
        # 跳过系统库（/lib, /usr/lib, /usr/lib/x86_64-linux-gnu）
        if(NOT _lib_path MATCHES "^/(lib|usr/lib($|/))")
            if(EXISTS "${_lib_path}")
                # 获取库文件名
                get_filename_component(_lib_name "${_lib_path}" NAME)
                # 复制库文件（跟随符号链接链）
                file(COPY "${_lib_path}" DESTINATION "${PACKAGE_DIR}/lib"
                     FOLLOW_SYMLINK_CHAIN)
            endif()
        endif()
    endif()
endforeach()

# 复制第三方库
if(EXISTS "${THIRD_PARTY_LIB_DIR}")
    file(GLOB _third_party_libs "${THIRD_PARTY_LIB_DIR}/*.so*")
    if(_third_party_libs)
        file(COPY ${_third_party_libs} DESTINATION "${PACKAGE_DIR}/lib")
    else()
        message(WARNING "No third-party libs found in: ${THIRD_PARTY_LIB_DIR}")
    endif()
else()
    message(WARNING "Third-party lib dir not found: ${THIRD_PARTY_LIB_DIR}")
endif()

# 复制 music 目录
if(DEFINED MUSIC_DIR AND EXISTS "${MUSIC_DIR}")
    file(COPY "${MUSIC_DIR}" DESTINATION "${PACKAGE_DIR}")
endif()

# 创建启动脚本
file(WRITE "${PACKAGE_DIR}/run.sh"
"#!/bin/bash
# SoundBridge portable launcher
SCRIPT_DIR=\"\$(cd \"\$(dirname \"\${BASH_SOURCE[0]}\")\" && pwd)\"
export LD_LIBRARY_PATH=\"\${SCRIPT_DIR}/lib:\${LD_LIBRARY_PATH}\"
exec \"\${SCRIPT_DIR}/SoundBridge\" \"\$@\"
")
file(CHMOD "${PACKAGE_DIR}/run.sh" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
     GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

message(STATUS "package_portable done")
message(STATUS "Run with: ${PACKAGE_DIR}/run.sh")
