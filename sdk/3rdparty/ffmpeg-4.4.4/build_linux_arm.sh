#!/bin/bash
# bash.exe build.sh windows amd64 gcc
system_name=$1
system_process=$2
toolchain=$3
install_dir=$4
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

if [ "$system_name" == "windows" ];
then
    MAKE=mingw32-make
else
    MAKE=make
fi

root_dir="$script_dir"
prefix_dir="$script_dir/../dist/$install_dir"
echo "[$system_name]安装目录为：$prefix_dir"

echo "ffmpeg_build: system_name is $system_name"
echo "ffmpeg_build: system_process is $system_process"
echo "ffmpeg_build: toolchain is $toolchain"
echo "ffmpeg_build: prefix is $prefix_dir"
echo "ffmpeg_build: build_dir is $root_dir"

cd "$root_dir"
echo "start config"
./configure --pkg-config-flags="" \
    --cross-prefix=arm-linux-gnueabihf- \
    --target-os=linux \
    --arch=arm \
    --prefix="$prefix_dir" \
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
