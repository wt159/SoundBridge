@echo off
setlocal enabledelayedexpansion

set system_name=%1
set system_process=%2
set toolchain=%3
set prefix=../dist/%system_name%_%system_process%_%toolchain%
set build_dir=../build
set boost_dir=%cd%\..\boost_1_81_0
cd %boost_dir%

echo boost_build: system_name is %system_name%
echo boost_build: system_process is %system_process%
echo boost_build: toolchain is %toolchain%
echo boost_build: prefix is %prefix%
echo boost_build: build_dir is %build_dir%
echo boost_build: boost_dir is %boost_dir%

set cores=0
for /f "skip=2" %%c in ('wmic cpu get NumberOfCores') do (
    set /a cores+=1
)
:end_cores
set processors=0
for /f "skip=1" %%p in ('wmic cpu get NumberOfLogicalProcessors') do (
    set /a processors=%%p
    goto end_processors
)
:end_processors
set /a max_threads=%cores% * %processors%
echo boost_build: cores is %cores%
echo boost_build: processors is %processors%
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
@REM --build-type=complete variant=release threading=multi link=shared runtime-link=shared
b2.exe -j %max_threads% ^
    --build-dir=%build_dir% --prefix=%prefix% ^
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