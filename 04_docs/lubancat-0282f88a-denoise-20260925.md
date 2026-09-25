# cam02 原生 RGB 降噪验证与部署

2026-09-25，用户要求在保留 RGB 1280×800、深度 320×180、30 FPS、Color 自动曝光和 5 ms 曝光上限的条件下减轻彩色噪点。

## 处理和依据

原始相机 JPEG 已有彩色颗粒，说明问题在 H.264 编码前存在。短曝光下原生 AE 会使用增益，增益会放大噪声；本次没有通过延长曝光或降低分辨率减噪，也没有证明所有噪声都来自同一个环节。

Orbbec SDK 2.8.6 的 `OB_PROP_COLOR_DENOISING_LEVEL_INT`（5525）描述为 Color camera CCI denoising：**0 是自动，1–8 强度递增；AE 关闭时无效**。相机初值为 0，可读写，范围 0–8、步长 1。最终部署 **4 档**，由相机处理照片和视频共有的 RGB 输入，不增加主机软件滤波或重新压缩照片。

`05_tools/orbbec_ae_reference_setup.cpp` 增加可选 `--denoise-level 0..8`：

- 保持原 `--serial SN --reference 0|1` 接口；省略降噪参数时不读写该属性。
- 严格按唯一序列号选设备；所有参数在 SDK 初始化前检查。
- 在写任何值前检查所有请求属性的读写权限、范围和步长；写后分别读回。不匹配返回失败，不声称自动恢复。
- 使用原有 systemd 启动项加载 `--reference 1 --denoise-level 4`；实际运行的发送端二进制、硬件 MPP 插件和 sender.json 的哈希均未变化。

## 图像对比

用户选择使用当前画面。对相机当时的实验台静止场景依次采集自动 A、4 档、8 档、自动 B，再补采 2 档和自动 C。每组 13 秒、29 张原始 JPEG，均为 1280×800、AE=1、曝光 raw=50（5 ms）。增益分别为 36、40、39、39、39、37；因此这是实际 AE 工作条件下的比较，不是严格锁定增益的实验室噪声测量。

所有组稳定采集约 30 FPS，无源帧号缺失；开流初段包含等待，不能把整段包含启动的约 29 FPS 误报为稳态降帧。

灰色秤体和浅色墙面采用相邻帧 Cb/Cr 差分 RMS 的中位数观察色彩波动；它也受光照、轻微运动和 JPEG 编码影响。以自动 B 为对照，4 档两块区域的指标分别下降约 **18.8% / 25.7%**。这不是“整幅彩噪降低 26%”的结论，也不表示所有暗部都同幅度改善。

滤波存在细节代价：瓶身小字区域的平均图像亮度梯度 P95，自动 B 为 39.30、2 档为 34.18、4 档为 28.38、8 档为 17.12。该数值是局部边缘指标，不是分辨率或 MTF。目视也能看到 8 档更明显的柔化。2 档在灰色区域的色彩波动改善不稳定，最终采用 4 档作为当前场景的降噪与细节折中。

原始照片、对比图保留在本地，不加入 Git。`comparison.json` 保存全部六组指标和 ROI 坐标；`compare_noise.py` 是离线分析脚本，需要同目录 `capture/` 中的原图和 CSV。

## 部署和验证

安装的独立 helper：

`/opt/gwv3/camera-controls/ae-denoise-f02b9fbbec70/ae_reference_setup`

原生 ARM 构建使用 SDK 2.8.6、C++17、`-O2 -Wall -Wextra -Wpedantic`。旧 helper 的降噪选项检查先失败；新版通过帮助文本及 15 个非法参数用例。旧、新接口指定不存在序列号均返回 3、匹配数 0。真实设备写入、独立进程重开读回及省略降噪参数的兼容调用均验证通过。systemd 启动日志再次确认 `reference=1`、`denoise=4`。

备份原启动项后，正常停止服务、部署新 helper 和启动项并重新启动。发送配置、RGB 2 Mbps 码率、深度 zlib、全范围标记和其他启动项保持。录制测试前确认采集、发送已进入稳态。

NAS 录像 `lubancat-0282f88a_cam02/2026-09-25/112331`：**66.045733 秒**。

| 指标 | RGB | 深度 |
|---|---:|---:|
| 实际尺寸 | 1280×800 | 320×180 |
| 有效帧 | 1983 | 1983 |
| 覆盖率 | 100.0822% | 100.0822% |
| 首帧延迟 | 13.867 ms | 12.228 ms |
| 尾帧延迟 | 3.894 ms | 15.787 ms |
| 最大间隔 | 45.509 ms | 45.551 ms |
| 源帧号缺失/重复/回退 | 0/0/0 | 0/0/0 |

两路采集跨度差 10.254 ms。覆盖率沿用按标称 30 FPS 计算的现有算法，窗口边界及帧率偏差使数值略高于 100%；未放宽 ≥98% / ≤500 ms 门槛。RGB、FFV1 gray16le 深度和音频全部解码通过，RGB CSV 帧索引/PTS 和标定尺寸匹配。全部 1983 张 RGB 帧 AE=1、raw exposure=50、gain=39，额外的 ≤5 ms 检查通过。深度接收延迟抽样 15.2–32.2 ms，录制队列无持续积压。

最终原始照片：`voice_photos/lubancat-0282f88a_cam02/2026-09-25/11-23-40/20260925_112340.jpg`，确认为 1280×800。

## 限制与回退

滤波减轻噪点但会柔化细纹理，不能恢复已经丢失的细节或保证低照度完全无噪点。本轮主要是静止场景，没有验证所有暗光、快速运动或降噪时域拖影情形；短录像也不能替代长时间及自动切片验证。音频归档已回归，未重新执行真实语音命令或物理按键测试。

启动项在 systemd 服务启动时执行；内部 watchdog 仅重启子进程不会重新执行 ExecStartPre。当前相机在进程重开后保留设置，未据此声称硬件断电永久保存。

先正常停止相机录像并等候收尾，再恢复自动降噪及旧启动项：

```sh
sudo bash -s <<'ROLLBACK'
set -eu
systemctl stop gwv3-gemini-sender.service
/usr/bin/timeout --kill-after=2s 15s \
  /opt/gwv3/camera-controls/ae-denoise-f02b9fbbec70/ae_reference_setup \
  --serial CV275610004S --reference 1 --denoise-level 0
install -m 0644 /var/backups/gwv3-denoise-20260925/70-color-ae-reference.before.conf \
  /etc/systemd/system/gwv3-gemini-sender.service.d/70-color-ae-reference.conf
systemctl daemon-reload
systemctl start gwv3-gemini-sender.service
ROLLBACK
```

只恢复旧启动项不会主动撤销仍通电相机的 4 档，因此回退明确写回 0。相关配置、验证和哈希在 `06_configs/deployment/lubancat-0282f88a-denoise-20260925/`。
