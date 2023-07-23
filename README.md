# SoundBridge_MusicPlayer

一款跨平台的音乐播放器

## 架构图

```shell
+-----------------+
|     用户界面     |
+-----------------+
         |
         |
+-----------------+
|     控制器      |
+-----------------+
| - 播放器        |
| - 音频引擎      |
| - 播放列表管理  |
| - 音乐库管理    |
+-----------------+
         |
         |
+-----------------+
|     播放器      |
+-----------------+
| - 音频解码器    |
| - 音频渲染器    |
| - 播放器状态    |
+-----------------+
         |
         |
+-----------------+
|     音频引擎     |
+-----------------+
| - 音频设备管理  |
| - 音频混合器    |
+-----------------+
```

在这个架构中，用户界面负责显示和处理用户输入，控制器负责管理播放器和音频引擎，播放器负责解码和渲染音频，音频引擎负责管理音频设备和混合音频。

具体实现时，您可以使用MVC模式或其他设计模式来组织代码。例如，用户界面可以使用Qt跨平台GUI库来实现，控制器可以使用C++类来封装业务逻辑，播放器可以使用现有的音频库或自行编写音频解码器和渲染器，音频引擎可以使用现有的音频库或自行编写音频设备管理和混合器。

## 下载

```shell
git clone git@github.com:wt159/SoundBridge.git
```

## 编译

### windows

#### 第三方库

第三方库一般是不用编译的，我这里是用mingw32编译器直接编译好的，在`SoundBridge/sdk/3rdparty/dist`目录下直接使用即可。

如果本机编译器不是这个编译器，需要新建`toolchain.windows_xxx.cmake`(可以按照`toolchain.windows_mingw.cmake`修改),然后重新编译。

```shell
# 打开cmd
cd SoundBridge/sdk/3rdparty
# 新建build目录
mkdir build && cd build
# 修改 toolchain.windows_mingw.cmake编译工具链
cmake .. --no-warn-unused-cli -DCMAKE_BUILD_TYPE:STRING=Debug -DCMAKE_TOOLCHAIN_FILE=%cd%\..\..\..\toolchain.windows_mingw.cmake -G "MinGW Makefiles"
```

#### soundBridge

```shell
# 打开cmd
cd SoundBridge/sdk/3rdparty
# 新建build目录
mkdir build && cd build
# 修改 toolchain.windows_mingw.cmake编译工具链
cmake .. --no-warn-unused-cli -DCMAKE_BUILD_TYPE:STRING=Debug -DCMAKE_TOOLCHAIN_FILE=%cd%\..\toolchain.windows_x86_64_mingw.cmake -G "MinGW Makefiles"
```
