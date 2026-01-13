# 已重构，新项目地址：https://github.com/rhodesepass/drm_app_neo

# 电子通行证播放器程序

调用全志Cedar库，实现通行证视频播放/切换等功能。

# 编译方法

需要提前准备的其他源码：

* 本项目的buildroot https://github.com/inapp123/buildroot-epass
* 全志的libcedarx https://github.com/EmperG/lindenis-v536-package/tree/master/allwinner/tina_multimedia/libcedarx

构建:
1. 拉取上文提到的buildroot,按repo中readme编译一次 编译工具链及依赖库
2. 运行source ./output/host/environment-setup 将生成的工具链和依赖设置为默认工具链和依赖
3. 将‎ board/rhodesisland/epass/rootfs/usr/lib/ 中所有文件拷贝至 output/host/arm-buildroot-linux-gnueabi/sysroot/usr/lib/
4. 将本repo clone至buildroot目录下
5. 使用[DownGit](https://minhaskamal.github.io/DownGit/#/home)等工具下载上文提到的libcedarx,将libcedarx文件夹放置在本repo文件夹下
6. 修改CMakeLists.txt,将其中所有include_directories换成自己的路径
若完全按照上文方式操作，可以用此配置
```
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../output/host/arm-buildroot-linux-gnueabi/sysroot/usr/include/drm/)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/libcedarx/libcore/base/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/libcedarx/libcore/parser/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/libcedarx/libcore/stream/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/libcedarx/external/include/adecoder)
```
7. 在本repo目录下运行cmake . && make,若正常则终端显示此日志且本repo目录中出现mediaplayerd二进制文件
```
[100%] Built target mediaplayerd
```
至此编译环境搭建完毕，可以使用此环境进行进一步开发
