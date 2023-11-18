# boost
if(NOT BUILD_OPTION_BOOST)
    option(BUILD_OPTION_BOOST "Build boost" OFF)
endif()
# ffmpeg
if(NOT BUILD_OPTION_FFMPEG)
    option(BUILD_OPTION_FFMPEG "Build ffmpeg" OFF)
endif()
# flac
if(NOT BUILD_OPTION_FLAC)
    option(BUILD_OPTION_FLAC "Build flac" OFF)
endif()
# g3log
if(NOT BUILD_OPTION_G3LOG)
    option(BUILD_OPTION_G3LOG "Build g3log" OFF)
endif()
# g3sinks
if(NOT BUILD_OPTION_G3SINKS)
    option(BUILD_OPTION_G3SINKS "Build g3sinks" OFF)
endif()
# zlib
if(NOT BUILD_OPTION_ZLIB)
    option(BUILD_OPTION_ZLIB "Build zlib" OFF)
endif()
# SDL
if(NOT BUILD_OPTION_SDL2)
    option(BUILD_OPTION_SDL2 "Build SDL2" OFF)
endif()
# ogg
if(NOT BUILD_OPTION_OGG)
    option(BUILD_OPTION_OGG "Build ogg" ON)
endif()
# vorbis
if(NOT BUILD_OPTION_VORBIS)
    option(BUILD_OPTION_VORBIS "Build vorbis" ON)
endif()