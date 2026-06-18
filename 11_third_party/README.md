# 第三方依赖说明

## 1. 目录定位

本目录用于说明和放置 `wireless_video_delivery` 工程需要的第三方依赖。
当前只放说明文件，不直接内置大型 SDK 或动态库实体。

项目总说明见 [../README.md](../README.md)，部署依赖见 [../04_docs/04_部署与运行手册.md](../04_docs/04_部署与运行手册.md)。

## 2. Orbbec SDK

发送端需要使用 Orbbec C/C++ SDK 调用 Gemini 深度相机。

已确认 Orbbec SDK v1.10.27 release 中包含 Linux ARM64 包，可作为树莓派 5 / 香橙派发送端的优先候选版本：

```text
https://github.com/orbbec/OrbbecSDK/releases/tag/v1.10.27
```

Gitee 镜像页面也能看到同版本下载列表：

```text
https://gitee.com/orbbecdeveloper/OrbbecSDK/releases/tag/v1.10.27
```

页面中和本项目相关的包如下：

```text
OrbbecSDK_C_C++_v1.10.27_20250925_0549823_linux_arm64_release.zip
OrbbecSDK_C_C++_v1.10.27_20250925_0549823_linux_x64_release.zip
OrbbecViewer_v1.10.27_202509260950_arm64_release.zip
OrbbecViewer_v1.10.27_202509260133_linux_x64_release.zip
```

说明：

1. `linux_arm64` SDK 给发送端使用，目标是树莓派 5 / 香橙派这类 ARM64 Linux 设备。
2. `linux_x64` SDK 给 Ubuntu 24.04 x86_64 接收端或开发机参考使用。
3. `OrbbecViewer` 是调试工具，不是项目运行时必须依赖；现场排查相机枚举、分辨率、帧率、固件状态时可以带上。
4. Gitee 页面中 `C++` 可能被显示成空格，最终以实际下载文件名为准。

## 3. 本地参考 SDK 规则

如果现场机器上已有旧版 Linux x64 SDK，只能用于：

1. 查看 C/C++ API。
2. 查看示例代码。
3. 参考 Linux x86_64 环境下的编译方式。

本仓库文档不记录个人 Windows 路径或某台机器上的临时 SDK 路径。旧版 x64 SDK 不应作为树莓派 5 / 香橙派发送端 SDK 使用。

## 4. 建议放置方式

如果后续确认要把 SDK 实体放进本工程，必须按架构分开：

```text
11_third_party/
  orbbec/
    linux_x64/
      README.md
      OrbbecSDK/
      OrbbecViewer/

    linux_arm64/
      README.md
      OrbbecSDK/
      OrbbecViewer/
```

规则：

1. `linux_x64` 只给接收端或 x86_64 开发机参考。
2. `linux_arm64` 才给树莓派 5 / 香橙派发送端使用。
3. 不允许在发送端构建脚本中引用 `linux_x64` SDK。
4. 不允许只放一个没有架构标识的 `SDK/` 目录。

## 5. 当前第三方依赖分工

当前主线已经围绕以下依赖组织：

1. Orbbec SDK：发送端相机采集和相机参数读取。
2. GStreamer + Rockchip MPP：发送端 RGB H.264 硬件编码。
3. FFmpeg：接收端 RGB/Depth 封装和导出工具依赖。
4. OpenCV：发送端本地预览和部分图像处理。
5. jsoncpp：C++ 配置和状态 JSON 处理。
6. zlib：Depth 无损压缩传输。
7. FastAPI：Web Monitor 服务。

后续如果引入 LZ4、Zstd、SRT、WebRTC 或新的硬件编码路径，应先在技术路线文档中说明原因、收益、风险和当前实现状态。

## 6. 当前不内置 SDK 实体的原因

当前没有直接复制 Orbbec SDK 实体文件，原因如下：

1. SDK 体积较大，打包前需要确认是否允许随项目分发。
2. 发送端和接收端所需架构不同，必须分开管理。
3. 先用文档明确版本和目录规则，避免后续工程师误用。

如果必须随项目一起交付 SDK，请把 ARM64 和 x64 两个包分别放入第 4 节建议目录，不要只命名为：

```text
OrbbecSDK.zip
```

这种命名无法区分平台，容易导致误用。
