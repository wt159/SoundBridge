#!/bin/bash
# bash.exe build.sh windows amd64 gcc
system_name=$1
system_process=$2
toolchain=$3

if [ "$system_name" == "windows" ];
then
    current_dir=$(pwd)
    echo "[$system_name]当前目录为：$current_dir"
    prefix_dir=$current_dir/../dist/$system_name"_"$system_process"_"$toolchain
    echo "[$system_name]安装目录为：$prefix_dir"

    echo "start config"
    ./configure --pkg-config-flags=""\
        --prefix=$prefix_dir \
        --enable-shared --enable-gpl --enable-version3 \
        --enable-nonfree --disable-libx264 --disable-libx265 \
        --disable-libvpx --disable-libopus --disable-libmp3lame \
        --disable-x86asm
    
    echo "start make"
    mingw32-make V=1 -j

    echo "start make install"
    mingw32-make install
else
    current_dir="$PWD"
    echo "[$system_name]当前目录为：$current_dir"
fi