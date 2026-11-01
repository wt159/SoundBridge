
# 3rdparty

## Windows

前置：环境变量 `QT_ROOT_PATH` 指向 Qt 根目录（如 `D:\Qt`）；`PATH` 内含
`<QtRoot>\Tools\mingw730_64\bin`（提供 `g++`、`mingw32-make`）与 Git 的 `bin`（提供 `bash`）。
手工配置容易漏项，推荐直接执行仓库根目录的 `scripts/setup_windows_env.ps1`。

```shell
# 打开cmd
cd SoundBridge/sdk/3rdparty
# 新建build目录
mkdir build && cd build
# 修改 toolchain.windows_x86_64_mingw.cmake编译工具链
cmake .. --no-warn-unused-cli \
    -DCMAKE_BUILD_TYPE:STRING=Debug \
    -DCMAKE_TOOLCHAIN_FILE=%cd%\..\..\..\cmake\toolchain\toolchain.windows_x86_64_mingw.cmake \
    -G "MinGW Makefiles" \
    -DVERSION=2.1.0 \
    -DBUILD_OPTION_BOOST=ON \
    -DBUILD_OPTION_FFMPEG=ON \
    -DBUILD_OPTION_FLAC=ON \
    -DBUILD_OPTION_OGG=ON \
    -DBUILD_OPTION_VORBIS=ON \
    -DBUILD_OPTION_SDL2=ON \
    -DBUILD_OPTION_ZLIB=ON
# 编译
cmake --build .
cmake --install .
```

### Windows 注意事项

- **`-DVERSION=2.1.0` 必须带上**：g3log 用 `git rev-list` 推导版本号，这条命令的嵌套引号在
  Windows 上不可靠，取不到值时 `math()` 会直接报错；显式传入可跳过该分支。
- **Boost 要静态 + 动态两套**：`g3sinks/CMakeLists.txt` 里写死 `Boost_USE_STATIC_LIBS OFF`，
  所以 `windows_build.bat` 用 `link=static,shared` 同时产出两种；若只有静态库，g3sinks 会报
  `No suitable build variant has been found`。
- **FFmpeg 的 DLL 副本**：默认用 `ln -s` 建符号链接，Windows 未开启开发者模式会失败；
  `ffmpeg-4.4.4/build.sh` 已加 `--ln_s="cp -f"` 改为复制。
- **Boost DLL 要挪到 bin**：b2 把动态库装到 `dist/<platform>/lib`，而运行期和 `package_portable`
  都从 `dist/<platform>/bin` 找，安装后需把 `lib/*.dll` 复制到 `bin/`。
- **G3LOG 默认开启**：`build_option.cmake` 里已把 `BUILD_OPTION_G3LOG` 默认值改为 ON，
  因为 g3sinks 依赖 g3log 的头文件；单独开 g3sinks 会找不到 `g3log/time.hpp`。

## Linux

* *ubuntu 20.04 LTS* 

```shell
cd SoundBridge/sdk/3rdparty
mkdir build && cd build
cmake .. --no-warn-unused-cli -DCMAKE_BUILD_TYPE:STRING=Debug \
    -DCMAKE_TOOLCHAIN_FILE=../../../cmake/toolchain/toolchain.linux_x86_64_gcc.cmake \
    -G "Unix Makefiles" \
    -DBUILD_OPTION_BOOST=ON \
    -DBUILD_OPTION_FFMPEG=ON \
    -DBUILD_OPTION_FLAC=ON \
    -DBUILD_OPTION_OGG=ON \
    -DBUILD_OPTION_VORBIS=ON \
    -DBUILD_OPTION_SDL2=ON \
    -DBUILD_OPTION_ZLIB=ON
cmake --build .
cmake --install .
```

### 注意事项

**Linux 平台编译 SDL 时强制依赖 PulseAudio**。若未安装，cmake 配置阶段会报错并提示安装命令：

```shell
# Ubuntu/Debian
sudo apt install libpulse-dev
# Fedora/RHEL
sudo dnf install pulseaudio-libs-devel
# Arch Linux
sudo pacman -S libpulse
```

Ubuntu下编译需要安装一些依赖：

[ubuntu常用编译依赖](https://wt159.github.io/2022/11/13/ubuntu%E5%BC%80%E5%8F%91%E7%8E%AF%E5%A2%83%E9%85%8D%E7%BD%AE.html)

[编译qt](https://wt159.github.io/2023/11/26/ubuntu%E4%B8%8B%E7%BC%96%E8%AF%91qt5.14.2%E6%BA%90%E7%A0%81.html)

[SDL2运行依赖](https://blog.csdn.net/qq_40017011/article/details/119748492)


## Embedded_linux

* imx6ull linux arm-linux-gnueabihf

```shell
cd SoundBridge/sdk/3rdparty
mkdir arm_build && cd arm_build
cmake .. --no-warn-unused-cli \
    -DCMAKE_BUILD_TYPE:STRING=Debug \
    -DCMAKE_TOOLCHAIN_FILE=../../../cmake/toolchain/toolchain.linux_arm_gnueabihf_gcc.cmake \
    -G "Unix Makefiles" \
    -DBUILD_OPTION_BOOST=ON \
    -DBUILD_OPTION_FFMPEG=ON \
    -DBUILD_OPTION_FLAC=ON \
    -DBUILD_OPTION_OGG=ON \
    -DBUILD_OPTION_VORBIS=ON \
    -DBUILD_OPTION_SDL2=ON \
    -DBUILD_OPTION_ZLIB=ON \
    -DBUILD_OPTION_G3LOG=OFF \
    -DBUILD_OPTION_G3SINKS=OFF 
cmake --build .
cmake --install .
```
