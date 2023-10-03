cmake_minimum_required(VERSION 3.2)

project(SoundBridge)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_AUTOMOC ON)   # moc转换支持
set(CMAKE_AUTOUIC ON)   # ui转换支持
set(CMAKE_AUTORCC ON)   # 资源文件转换支持

# set(QT_MESSAGELOGCONTEXT TRUE) # Qt消息日志打开
# add_compile_definitions(-DQT_MESSAGELOGCONTEXT)

set(APP_SOURCES
    main.cpp
    mainwindow.cpp
    mainwindow.h
    res.qrc
)

include_directories(${QT_LIB_PATH}/../include/QtWidgets)
include_directories(${QT_LIB_PATH}/../include/QtCore)
include_directories(${QT_LIB_PATH}/../include/QtGui)
include_directories(${QT_LIB_PATH}/../include/QtMultimedia)
include_directories(${CMAKE_CURRENT_LIST_DIR}/../sdk)
link_directories(${3RDPARTY_DIST_DIR}/lib)
link_directories(${3RDPARTY_DIST_DIR}/bin)
message(STATUS "3RDPARTY_DIST_DIR: ${3RDPARTY_DIST_DIR}")

set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH} ${QT_LIB_PATH})
find_package(Qt5 COMPONENTS Widgets REQUIRED)
find_package(Qt5 COMPONENTS Multimedia REQUIRED)

add_executable(${PROJECT_NAME} ${APP_SOURCES})

set(SDK_LIBS
    sdk 
    Extractor
    WavExtractor
    AudioDevice 
    AudioDecode 
    AudioResample 
    LogWrapper)

target_link_libraries(${PROJECT_NAME} Qt5::Widgets Qt5::Multimedia ${SDK_LIBS})