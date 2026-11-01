# boost
if(NOT DEFINED BUILD_OPTION_BOOST)
    option(BUILD_OPTION_BOOST "Build boost" OFF)
endif()
# ffmpeg
if(NOT DEFINED BUILD_OPTION_FFMPEG)
    option(BUILD_OPTION_FFMPEG "Build ffmpeg" ON)
endif()
# flac
if(NOT DEFINED BUILD_OPTION_FLAC)
    option(BUILD_OPTION_FLAC "Build flac" ON)
endif()
# g3log
if(NOT DEFINED BUILD_OPTION_G3LOG)
    # g3sinks 依赖 g3log 的头文件与库，默认一起构建，避免单独开启 g3sinks 时找不到 g3log
    option(BUILD_OPTION_G3LOG "Build g3log" ON)
endif()
# g3sinks
if(NOT DEFINED BUILD_OPTION_G3SINKS)
    option(BUILD_OPTION_G3SINKS "Build g3sinks" OFF)
endif()
# zlib
if(NOT DEFINED BUILD_OPTION_ZLIB)
    option(BUILD_OPTION_ZLIB "Build zlib" ON)
endif()
# SDL
if(NOT DEFINED BUILD_OPTION_SDL2)
    option(BUILD_OPTION_SDL2 "Build SDL2" ON)
endif()
# ogg
if(NOT DEFINED BUILD_OPTION_OGG)
    option(BUILD_OPTION_OGG "Build ogg" ON)
endif()
# vorbis
if(NOT DEFINED BUILD_OPTION_VORBIS)
    option(BUILD_OPTION_VORBIS "Build vorbis" ON)
endif()
