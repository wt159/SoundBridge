# SoundBridge_MusicPlayer

一款跨平台的音乐播放器, 用来学习各类音频文件格式(aac ogg wav aiff mp3 flac m4a asf wma)的解析, 及相应的解码流程。

## 架构图

```shell
+---------------------------------------------+
|                     APP                     |
+---------------------------------------------+
                       |
                       |
+---------------------------------------------+
|                     SDK                     |
+---------------------------------------------+
|           - MusicPlyer                      |
|               - Log                         |
|               - Audio                       |
|               - Extractor                   |
+---------------------------------------------+
```

## 下载

```shell
git clone git@github.com:wt159/SoundBridge.git
```

## 编译

[**注意第一次编译请先编译sdk/3rdparty目录下的依赖库，点击跳转**](sdk/3rdparty/Readme.md)

### windows

#### soundBridge

```shell
# 打开cmd
cd SoundBridge
# 新建build目录
mkdir build && cd build
# 修改 toolchain.windows_x86_64_mingw.cmake编译工具链
cmake .. --no-warn-unused-cli -DCMAKE_BUILD_TYPE:STRING=Debug -DCMAKE_TOOLCHAIN_FILE=%cd%\..\cmake\toolchain\toolchain.windows_x86_64_mingw.cmake -G "MinGW Makefiles"
# 编译
cmake --build .
```

### Linux

#### soundBridge

```shell
# 打开bash
cd SoundBridge
# 新建build目录
mkdir build && cd build
# 指定编译工具链
cmake .. --no-warn-unused-cli -DCMAKE_BUILD_TYPE:STRING=Debug -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain/toolchain.linux_x86_64_gcc.cmake -G "Unix Makefiles"
# 编译
cmake --build .
```

### Embedded_Linux

```shell
# 打开bash
cd SoundBridge
# 新建build目录
mkdir build && cd build
# 指定编译工具链
cmake .. --no-warn-unused-cli -DCMAKE_BUILD_TYPE:STRING=Debug -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain/toolchain.linux_arm_gnueabihf_gcc.cmake -G "Unix Makefiles"
# 编译
cmake --build .
```

## 运行

待完成

## 进度

### 已完成

* 第三方库模块
* 日志log模块
* 音频播放设备模块
* 音频重采样模块
* 用户界面模块
* 音频解码模块
* 音频信息模块

### 进行中

* asf文件解码

### 待办

* m4a解码失败
