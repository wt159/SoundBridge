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

### windows

#### 3rdparty

第三方库一般是不用编译的，我这里是用mingw32编译器直接编译好的，在`SoundBridge/sdk/3rdparty/dist`目录下直接使用即可。

如果本机编译器不是这个编译器，需要新建`toolchain.windows_xxx.cmake`(可以按照`toolchain.windows_x86_64_mingw.cmake`修改),然后重新编译。

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

#### 3rdparty

第三方库一般是不用编译的，我这里是用gcc编译器直接编译好的，在`SoundBridge/sdk/3rdparty/dist`目录下直接使用即可。

如果本机编译器不是这个编译器，需要新建`toolchain.linux_xxx.cmake`(可以按照`toolchain.linux_x86_64_gcc.cmake`修改),然后重新编译。

```shell
cd SoundBridge/sdk/3rdparty
mkdir build && cd build
cmake .. --no-warn-unused-cli -DCMAKE_BUILD_TYPE:STRING=Debug -DCMAKE_TOOLCHAIN_FILE=../../../cmake/toolchain/toolchain.linux_x86_64_gcc.cmake -G "Unix Makefiles"
cmake --build .
cmake --install .
```

#### soundBridge

```shell
# 打开cmd
cd SoundBridge
# 新建build目录
mkdir build && cd build
# 修改 toolchain.windows_x86_64_mingw.cmake编译工具链
cmake .. --no-warn-unused-cli -DCMAKE_BUILD_TYPE:STRING=Debug -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain/toolchain.linux_x86_64_gcc.cmake -G "Unix Makefiles"
# 编译
cmake --build .
```

### 嵌入式Linux

待完成

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
