@echo off
setlocal
if exist build (
    echo build directory already exists
) else (
    mkdir build
)

if exist ..\dist\windows_amd64_gcc (
    echo dist directory already exists
) else (
    mkdir ..\dist\windows_amd64_gcc
)

call bootstrap.bat toolset=gcc

b2.exe --build-dir=%cd%\build --prefix=%cd%\..\dist\windows_amd64_gcc --build-type=complete variant=release threading=multi link=shared runtime-link=shared toolset=gcc install

endlocal