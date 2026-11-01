@echo off
setlocal enabledelayedexpansion

set system_name=%1
set system_process=%2
set toolchain=%3
set install_dir=%4
set script_dir=%~dp0
set prefix=%script_dir%\..\dist\%install_dir%
set build_dir=%script_dir%\..\build
set boost_dir=%script_dir%
cd /d %boost_dir%

echo boost_build: system_name is %system_name%
echo boost_build: system_process is %system_process%
echo boost_build: toolchain is %toolchain%
echo boost_build: prefix is %prefix%
echo boost_build: build_dir is %build_dir%
echo boost_build: boost_dir is %boost_dir%

rem Use NUMBER_OF_PROCESSORS instead of wmic: wmic is removed on newer Windows
rem and cannot run in restricted environments. The env var works everywhere.
set max_threads=%NUMBER_OF_PROCESSORS%
if not defined max_threads set max_threads=4
echo boost_build: max_threads is %max_threads%

if exist %build_dir% (
    echo build directory already exists
) else (
    echo build directory not exists
    mkdir "%build_dir%"
)

if exist %prefix% (
    echo dist directory already exists
) else (
    echo dist directory not exists
    mkdir "%prefix%"
)

if exist %boost_dir%/b2.exe (
    echo boost_build: b2 already exists
) else (
    echo boost_build: b2 does not exist
    echo boost_build: bootstrap
    call bootstrap.bat %toolchain%
)

echo boost_build: b2
@REM g3sinks 里写死了 Boost_USE_STATIC_LIBS=OFF，因此静态/动态两套都要产出；
@REM address-model=64 限定只编 64 位，否则 b2 会尝试链接 32 位 DLL 而失败
b2.exe cxxflags="--std=gnu++11" -j %max_threads% ^
    --build-dir=%build_dir% --prefix=%prefix% ^
    address-model=64 link=static,shared runtime-link=shared ^
    --without-atomic ^
    --without-chrono ^
    --without-container ^
    --without-contract ^
    --without-coroutine ^
    --without-date_time ^
    --without-exception ^
    --without-fiber ^
    --without-graph ^
    --without-graph_parallel ^
    --without-headers ^
    --without-iostreams ^
    --without-locale ^
    --without-math ^
    --without-nowide ^
    --without-program_options ^
    --without-python ^
    --without-random ^
    --without-regex ^
    --without-stacktrace ^
    --without-test ^
    --without-thread ^
    --without-timer ^
    --without-type_erasure ^
    --without-url ^
    --without-wave ^
    toolset=%toolchain% install

endlocal
