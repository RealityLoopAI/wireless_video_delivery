# Third-Party Dependencies

本目录定义项目第三方依赖的版本边界和放置规则。大型 SDK、Viewer、动态库和安装包由 `.gitignore` 排除，不直接提交。

## Compatibility Baseline

| Dependency | Used by | Requirement |
| --- | --- | --- |
| Orbbec SDK | sender | 必须匹配相机型号、CPU 架构和目标设备运行时 |
| GStreamer | sender | 必须与 Rockchip MPP 插件 ABI 一致 |
| Rockchip MPP plugin | sender | 提供 `mpph264enc`，部分设备还使用 `mppjpegdec` |
| OpenCV | sender/tools | 本机预览和图像处理 |
| jsoncpp | C++ components | 配置和状态 JSON |
| zlib/LZ4 | sender/receiver | Depth 压缩与解压 |
| FFmpeg | receiver/tools | RGB/Depth 封装、探测和导出 |
| FastAPI | Web Monitor | 网页与 REST 代理 |

仓库中的“已验证”表示在特定硬件和镜像组合上测试通过，不表示上游最新版本，也不保证换型号后兼容。

## Orbbec SDK

已使用的基线：

| Camera/platform | SDK result |
| --- | --- |
| 既有 Gemini 设备 / ARM64 | v1.10.27 为保留兼容基线，部署前仍需实机枚举与 profile 测试 |
| Gemini 305 (`2bc5:0840`) / RK3576 ARM64 | v2.8.6 已验证可枚举并采集；v1.10.27 在该组合上不能枚举 |

参考官方 release：

- Orbbec SDK v1.10.27: <https://github.com/orbbec/OrbbecSDK/releases/tag/v1.10.27>
- Orbbec SDK v2.8.6: <https://github.com/orbbec/OrbbecSDK_v2/releases/tag/v2.8.6>

已验证的 v2.8.6 ARM64 包：

```text
OrbbecSDK_v2.8.6_202604271452_6399409_linux_arm64.tar.gz
SHA256: a052221d4bdea6afb2f8b338bcd6e635afffcebbacab1483422b986e680fb441
```

SDK 放置约定：

```text
11_third_party/
  orbbec/
    linux_arm64/
      OrbbecSDK_v2.8.6/
      <other-version>/
    linux_x64/
      <sdk-or-viewer>/
```

规则：

1. sender 只使用目标架构目录，不允许引用 `linux_x64` 作为 ARM64 运行时。
2. 不使用无版本、无架构的通用 `SDK/` 目录名。
3. 复制 sender 二进制时必须同步核对 SDK 动态库和配置文件，不能只复制可执行文件。
4. USB `lsusb` 能看到设备只证明枚举到 USB，不证明 SDK 能打开它。
5. 升级 SDK 后重新验证可选 profile、实际 FPS、曝光/白平衡属性和热插拔恢复。

## GStreamer And MPP

`libgstrockchipmpp.so` 文件存在不等于插件可用。部署时至少检查：

```bash
gst-inspect-1.0 mpph264enc
gst-inspect-1.0 mppjpegdec
```

如果插件被 blacklist，优先检查 GStreamer runtime 与插件构建 ABI。设备专用 `GST_PLUGIN_PATH_1_0` 可以指向已验证插件目录，但不能覆盖成另一镜像或另一 ABI 的文件。最终验收必须跑实际编码 pipeline，不能只依赖 `gst-inspect`。

## Dependency Changes

引入或升级 Zstd、SRT、WebRTC、SDK、codec 或硬件插件时，应在同一提交中记录：

- 解决的问题和选择该版本的原因；
- 支持的架构、系统镜像和硬件型号；
- license 与是否允许分发；
- 构建/运行时依赖及校验值；
- 实机功能、性能和回退测试。

部署依赖和 ABI 排查见 [deployment.md](../04_docs/deployment.md)，技术演进状态见 [roadmap.md](../04_docs/roadmap.md)。
