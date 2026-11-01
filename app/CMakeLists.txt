cmake_minimum_required(VERSION 3.5)

project(SoundBridge)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_AUTOMOC ON)   # moc转换支持
set(CMAKE_AUTOUIC ON)   # ui转换支持
set(CMAKE_AUTORCC ON)   # 资源文件转换支持

set(QT_MESSAGELOGCONTEXT TRUE) # Qt消息日志打开
add_definitions(-DQT_MESSAGELOGCONTEXT) # Qt消息日志打开
# add_compile_definitions(-DQT_MESSAGELOGCONTEXT)

set(APP_SOURCES
    main.cpp
    mainwindow.cpp
    mainwindow.h
    playercontroller.cpp
    playercontroller.h
    res.qrc
)

include_directories(${QT_LIB_PATH}/../include/QtWidgets)
include_directories(${QT_LIB_PATH}/../include/QtCore)
include_directories(${QT_LIB_PATH}/../include/QtGui)
# include_directories(${CMAKE_CURRENT_LIST_DIR}/../sdk)
link_directories(${3RDPARTY_DIST_DIR}/lib)
link_directories(${3RDPARTY_DIST_DIR}/bin)

# Qt5Config.cmake 位于 <Qt安装目录>/lib/cmake/Qt5，因此前缀要指向 Qt 的 mingw 根目录，
# 而不是它的 lib 子目录（保留 QT_LIB_PATH 以兼容已有缓存配置）。
set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH} ${QT_MINGW_PATH} ${QT_LIB_PATH})
find_package(Qt5 COMPONENTS Widgets REQUIRED)

add_executable(${PROJECT_NAME} WIN32 ${APP_SOURCES})

target_link_libraries(${PROJECT_NAME} Qt5::Widgets sdk)
