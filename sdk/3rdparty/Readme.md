
# 3rdparty

## Windows

```shell
# 打开cmd
cd SoundBridge/sdk/3rdparty
# 新建build目录
mkdir build && cd build
# 修改 toolchain.windows_x86_64_mingw.cmake编译工具链
cmake .. --no-warn-unused-cli -DCMAKE_BUILD_TYPE:STRING=Debug -DCMAKE_TOOLCHAIN_FILE=%cd%\..\..\..\cmake\toolchain\toolchain.windows_x86_64_mingw.cmake -G "MinGW Makefiles"
# 编译
cmake --build .
cmake --install .
```

## Linux

* *ubuntu 20.04 LTS* 

```shell
cd SoundBridge/sdk/3rdparty
mkdir build && cd build
cmake .. --no-warn-unused-cli -DCMAKE_BUILD_TYPE:STRING=Debug -DCMAKE_TOOLCHAIN_FILE=../../../cmake/toolchain/toolchain.linux_x86_64_gcc.cmake -G "Unix Makefiles"
cmake --build .
cmake --install .
```

### 注意事项

Ubuntu下编译需要安装一些依赖：

[ubuntu常用编译依赖](https://wt159.github.io/2022/11/13/ubuntu%E5%BC%80%E5%8F%91%E7%8E%AF%E5%A2%83%E9%85%8D%E7%BD%AE.html)

[编译qt](https://wt159.github.io/2023/11/26/ubuntu%E4%B8%8B%E7%BC%96%E8%AF%91qt5.14.2%E6%BA%90%E7%A0%81.html)

[SDL2运行依赖](https://blog.csdn.net/qq_40017011/article/details/119748492)


## Embedded_linux

* imx6ull linux arm-linux-gnueabihf

```shell
cd SoundBridge/sdk/3rdparty
mkdir arm_build && cd arm_build
cmake .. --no-warn-unused-cli -DCMAKE_BUILD_TYPE:STRING=Debug -DBUILD_OPTION_G3LOG=OFF -DBUILD_OPTION_G3SINKS=OFF -DCMAKE_TOOLCHAIN_FILE=../../../cmake/toolchain/toolchain.linux_arm_gnueabihf_gcc.cmake -G "Unix Makefiles"
cmake --build .
cmake --install .
```