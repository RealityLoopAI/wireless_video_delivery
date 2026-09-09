# Rockchip GStreamer 定向补丁

`gstreamer-rockchip-sync-point.patch` 仅修复编码器向 GStreamer 报告实际关键帧的标志。
不修改主程序、MPP 固件、码率、GOP、帧 PTS 或 TCP 协议。

- 上游源码：<https://github.com/JeffyCN/mirrors/tree/gstreamer-rockchip>
- 固定提交：`ca829e0dc1d814df01711a5796e7510812af9e85`
- 修改文件：`gst/rockchipmpp/gstmppenc.c`
- 许可：该文件及补丁沿用其文件头的 LGPL-2.0-or-later；分发二进制时保留上游
  `COPYING`、版权声明、对应源码和本补丁，不用本仓库其他许可替代。
- 构建：`05_tools/build_patched_mpp_plugin.sh CLEAN_SOURCE BUILD_DIRECTORY`。
  必须使用独立、干净的指定提交工作树；构建会给该工作树应用补丁。
  脚本不会安装插件或重启服务。
- 适用验证范围：本文日期已测试的 RK3588/RK3576、现有 MPP 开发库、H.264 路径。
  不是所有 Rockchip 镜像、编码格式和分辨率的兼容保证。

部署、测试和回退见 [修复报告](../../04_docs/mpp-keyframe-recovery-20260909.md)。
