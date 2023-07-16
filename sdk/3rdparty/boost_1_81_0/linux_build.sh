#!/bin/bash

system_name=$1
system_process=$2
toolchain=$3
install_dir=$4
prefix=../dist/${install_dir}
build_dir=../build
boost_dir=$(cd ../boost_1_81_0 && pwd)

cores=1
processors=$(grep -c "^processor" /proc/cpuinfo)
max_threads=$((cores * processors))
echo "boost_build: cores is $cores"
echo "boost_build: processors is $processors"
echo "boost_build: max_threads is $max_threads"
echo "boost_build: system_name is $system_name"
echo "boost_build: system_process is $system_process"
echo "boost_build: toolchain is $toolchain"
echo "boost_build: prefix is $prefix"
echo "boost_build: build_dir is $build_dir"
echo "boost_build: boost_dir is $boost_dir"
echo "boost_build: cd boost_dir"
cd "$boost_dir"

if [ -d "$build_dir" ]; then
    echo "build directory already exists"
else
    mkdir "$build_dir"
fi

if [ -d "$prefix" ]; then
    echo "dist directory already exists"
else
    mkdir "$prefix"
fi

if [ -f "$boost_dir/b2" ]; then
    echo "boost_build: b2 already exists"
else
    echo "boost_build: b2 does not exist"
    echo "boost_build: bootstrap"
    sh $boost_dir/bootstrap.sh $toolchain
fi

echo "boost_build: b2"
# --build-type=complete variant=release threading=multi link=shared runtime-link=shared
./b2 -j "$max_threads" \
    --build-dir="$build_dir" --prefix="$prefix" \
    --without-atomic \
    --without-chrono \
    --without-container \
    --without-contract \
    --without-coroutine \
    --without-date_time \
    --without-exception \
    --without-fiber \
    --without-graph \
    --without-graph_parallel \
    --without-headers \
    --without-iostreams \
    --without-locale \
    --without-math \
    --without-nowide \
    --without-program_options \
    --without-python \
    --without-random \
    --without-regex \
    --without-stacktrace \
    --without-test \
    --without-thread \
    --without-timer \
    --without-type_erasure \
    --without-url \
    --without-wave \
    toolset="$toolchain" install