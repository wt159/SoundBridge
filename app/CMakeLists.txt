cmake_minimum_required(VERSION 3.5)

project(SoundBridge)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_AUTOMOC ON)   # moc转换支持
set(CMAKE_AUTOUIC ON)   # ui转换支持
set(CMAKE_AUTORCC ON)   # 资源文件转换支持

set(QT_MESSAGELOGCONTEXT TRUE) # Qt消息日志打开
add_definitions(-DQT_MESSAGELOGCONTEXT) # Qt消息日志打开
# add_compile_definitions(-DQT_MESSAGELOGCONTEXT)

# ADR 0017（地图 #248 / 执行票 #257）：app 层生产源（不含 main.cpp）编进
# app_core STATIC，SoundBridge.exe 与 UI 测试宿主共用一份编译产物、一份 moc。
set(APP_CORE_SOURCES
    mainwindow.cpp
    mainwindow.h
    playercontroller.cpp
    playercontroller.h
    playerport.h
    popuppanel.cpp
    popuppanel.h
    res.qrc
)

set(APP_SOURCES
    main.cpp
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
# ADR 0017：QtTest 组件随 UI 测试宿主引入（qtbase 归档已含，双端 CI 无需新装包）。
find_package(Qt5 COMPONENTS Widgets Test REQUIRED)

add_library(app_core STATIC ${APP_CORE_SOURCES})
target_link_libraries(app_core PUBLIC Qt5::Widgets sdk)

add_executable(${PROJECT_NAME} WIN32 ${APP_SOURCES})

target_link_libraries(${PROJECT_NAME} app_core)

# ---- UI 测试宿主（ADR 0016 双缝 + ADR 0017 目标预算；判据真源 doc/app-ui-spec.md §9.7）----
set(APP_UI_TEST_SOURCES
    tests/AppUiTests.cpp
    tests/FakeCommandPort.hpp
)

add_executable(AppUiTests ${APP_UI_TEST_SOURCES})
target_include_directories(AppUiTests PRIVATE ${SDK_DIR}/test/fixtures)
target_compile_definitions(AppUiTests PRIVATE
    SB_APP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(AppUiTests Qt5::Widgets Qt5::Test sdk app_core)

# Windows 下 offscreen 平台插件不在 windeployqt 默认集，必须随测试 exe 部署
# （规格 §9.7.3：缺它进程停在致命对话框、不返回、零日志，timeout 只见 124）。
if(WIN32 AND QT_MINGW_PATH AND EXISTS "${QT_MINGW_PATH}/plugins/platforms/qoffscreen.dll")
    add_custom_command(TARGET AppUiTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:AppUiTests>/platforms"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${QT_MINGW_PATH}/plugins/platforms/qoffscreen.dll"
                "$<TARGET_FILE_DIR:AppUiTests>/platforms/qoffscreen.dll"
        COMMENT "Deploy qoffscreen platform plugin for AppUiTests")
elseif(WIN32)
    message(WARNING "qoffscreen.dll NOT deployed (QT_MINGW_PATH unset or plugin missing); "
        "sdk_app_ui_tests will hang at the fatal dialog with timeout 124, zero logs - spec 9.7.3")
endif()

# 第 15 个 ctest 目标（ADR 0017：14 → 15 一次性显式例外，已登记 sdk/test/AGENTS.md）。
# TIMEOUT 120 沿用坏输入四段档位值；offscreen + dummy 音频保证测试进程零真实设备。
add_test(NAME sdk_app_ui_tests COMMAND AppUiTests)
set_tests_properties(sdk_app_ui_tests PROPERTIES
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;SDL_AUDIODRIVER=dummy")
