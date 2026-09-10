# RGB 编码修复验收证据

代码：`8c174858d674`，sender source hash：`d76da5bfbf9ccb75`。

- `wvd-backpressure-before.txt`：修改前的失败测试，提交 8 帧仅返回 3/3/4 帧。
- `wvd-backpressure-after.txt`：第一轮修复后三条路径均完整返回 8 帧。
- `wvd-encoder-all-tests.txt`、`wvd-encoder-release-tests.txt`：本机 23 项测试，包含 12 项编码回归。
- `sanitizers.json`：12 项 ASan/UBSan 测试，均退出 0；关闭第三方库的泄漏检测，不能当作 LSan 验收。
- `sdk1/`、`sdk2/`：fe0f 和 52d2 的原生测试、MPP 专项测试及二进制部署散列。
- `rollout/`：其余四台的原机硬件 PTS 测试、运行版本和配置散列。
- `fe-root-test-failure/`：首次以 root 测试时的失败记录及自动回退结果，未删除失败证据。

旧压力测试使用等间隔 PTS；新测试另外覆盖抖动和 100ms 缺口，要求完整、唯一地保留输入 PTS。
预览丢帧测试中的 `missing>0` 是明确预期；正式流场景要求 `missing=0`。
`timed_out=1` 是持续拥塞测试的预期，随后还要验证已接收帧全部排空。

## fe0f root 测试失败

首次 root MPP 测试收到 SIGSEGV，脚本恢复原 `f9f5411dee7e` 二进制并重启服务，
没有将失败候选上线。GDB 再次以 root 复现，关键栈为：

```text
Thread "mpp_h264e_*":
mpp_buffer_create
mpp_buffer_get_with_tag
hal_bufs_get_buf
mpp_enc_async_thread
```

另一线程也在 `gst_mpp_allocator_alloc -> mpp_buffer_get_with_tag -> mpp_buffer_create`。
相关系统库为 `/lib/aarch64-linux-gnu/librockchip_mpp.so.1`，不是应用的时间戳匹配函数。
GDB 是临时复制到 `/tmp` 的工具，没有安装或替换系统 MPP/GStreamer 库。

以实际服务用户 `orangepi` 重跑后，12/12 MPP 场景及两项生产分辨率关键帧测试均通过，
随后才上线。root 与普通账号行为差异的底层原因尚未进一步证明，
不能把它说成本轮已修复的 MPP 库问题。生产服务继续使用原非 root 账号。

## 部署范围

六台在线 sender 已统一运行上述 commit/hash。SDK v1 候选在 fe0f、本机构建，
SDK v2 候选在 52d2 构建；相同 ARM64/SDK 运行环境的其余节点复用相应候选，
在原机执行配置校验、MPP PTS 测试及实时发送验收。不是声称在每台机器都重新编译。
该轮 sender 部署时 Receiver 为 `90da5c8bcb94`，未改相机配置或系统 MPP 插件。
各生产二进制旁均保留 `gemini_sender.before-encoder-pts-20260910`。

`recording.json` 为第一轮六路 300 秒结果，整体验收失败（52d2/e8 深度缺头）。
`recording-analysis.json` 包含 12 文件完整解码与 CSV 分析。
`sender-*.json` 为该录制窗口的发送端日志统计。
`prestart-before.txt` 是旧 Receiver 丢深度的隔离复现。
后续接收端修复证据在 `../recording-prestart-depth-20260910/`。
