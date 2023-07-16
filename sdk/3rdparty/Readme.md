
# 3rdparty

## windows

```cmd
cd D:\xxx\SoundBridge\sdk\3rdparty
mkdir build && cd build
cmake .. --no-warn-unused-cli -Wno-dev -DCMAKE_BUILD_TYPE:STRING=Debug -DCMAKE_TOOLCHAIN_FILE=%cd%\..\..\..\toolchain.windows_x86_64_mingw.cmake -G "MinGW Makefiles"
cmake --build %cd&
mingw32-make install
```

## ubuntu

```shell
cd SoundBridge/sdk/3rdparty
mkdir build && cd build
cmake .. --no-warn-unused-cli -Wno-dev -DCMAKE_BUILD_TYPE:STRING=Debug -DCMAKE_TOOLCHAIN_FILE=./../../../toolchain.linux_x86_64_gcc.cmake
cmake --build ./
make install
```
