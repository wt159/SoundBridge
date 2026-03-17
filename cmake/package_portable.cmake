if(NOT WIN32)
    message(FATAL_ERROR "package_portable is supported only on Windows")
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

if(NOT DEFINED WINDEPLOYQT_EXECUTABLE)
    message(FATAL_ERROR "WINDEPLOYQT_EXECUTABLE not set")
endif()
if(NOT EXISTS "${WINDEPLOYQT_EXECUTABLE}")
    message(FATAL_ERROR "windeployqt not found: ${WINDEPLOYQT_EXECUTABLE}")
endif()

if(NOT DEFINED THIRD_PARTY_DLL_DIR)
    message(FATAL_ERROR "THIRD_PARTY_DLL_DIR not set")
endif()

message(STATUS "Packaging to: ${PACKAGE_DIR}")
file(REMOVE_RECURSE "${PACKAGE_DIR}")
file(MAKE_DIRECTORY "${PACKAGE_DIR}")

file(COPY "${APP_EXE}" DESTINATION "${PACKAGE_DIR}")

execute_process(
    COMMAND "${WINDEPLOYQT_EXECUTABLE}" --force --compiler-runtime "${APP_EXE}"
    RESULT_VARIABLE _windeployqt_rv
)
if(NOT _windeployqt_rv EQUAL 0)
    message(FATAL_ERROR "windeployqt failed with code: ${_windeployqt_rv}")
endif()

if(EXISTS "${THIRD_PARTY_DLL_DIR}")
    file(GLOB _third_party_dlls "${THIRD_PARTY_DLL_DIR}/*.dll")
    if(_third_party_dlls)
        file(COPY ${_third_party_dlls} DESTINATION "${PACKAGE_DIR}")
    else()
        message(WARNING "No third-party DLLs found in: ${THIRD_PARTY_DLL_DIR}")
    endif()
else()
    message(WARNING "Third-party DLL dir not found: ${THIRD_PARTY_DLL_DIR}")
endif()

if(DEFINED MUSIC_DIR AND EXISTS "${MUSIC_DIR}")
    file(COPY "${MUSIC_DIR}" DESTINATION "${PACKAGE_DIR}")
endif()

message(STATUS "package_portable done")
