#!/bin/bash
# bash.exe build.sh windows amd64 gcc
system_name=$1
system_process=$2
toolchain=$3
install_dir=$4

if [ "$system_name" == "windows" ];
then
    current_dir=$(pwd)
    MAKE=mingw32-make
    echo "[$system_name]当前目录为：$current_dir"
    
else
    current_dir="$PWD"
    MAKE=make
    echo "[$system_name]当前目录为：$current_dir"
fi

root_dir=$current_dir/../ffmpeg-4.4.4
prefix_dir=$root_dir/../dist/$install_dir
echo "[$system_name]安装目录为：$prefix_dir"

cd $root_dir
echo "start config"
./configure --pkg-config-flags="" \
    --prefix=$prefix_dir \
    --enable-shared \
    --enable-gpl \
    --enable-version3 \
    --enable-nonfree \
    --disable-postproc \
    --disable-debug \
    --disable-doc \
    --disable-ffmpeg \
    --disable-ffplay \
    --disable-ffprobe \
    --disable-x86asm

echo "start make"
$MAKE V=1 #-j

echo "start make install"
$MAKE install