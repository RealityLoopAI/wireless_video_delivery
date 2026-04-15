# Windows 一键启动问题总结

日期: 2026-04-10  
目录: `E:\Win10\Desktop\wireless_video_delivery`

## 1. 现象总览

- 在 Windows 环境下尝试“一键启动”失败.
- 直接执行 `.\start_receiver_windows.ps1` 初始报错配置文件找不到.
- 首次自动装环境后再次启动, 报 `ModuleNotFoundError: No module named 'av'`.
- 依赖补装过程中出现 `uv` 全局缓存目录权限错误.
- 启动脚本有时误报 `Receiver exited too early`.

## 2. 根因分析

1. 一键入口不匹配平台
- 根目录已有 `one_click.sh`, 该脚本只覆盖 Linux 流程, Windows 无等价一键入口.

2. Windows 根启动脚本路径拼接错误
- `start_receiver_windows.ps1` 把相对路径传给 `05_tools` 脚本后, 被拼成错误路径:
- 错误路径示例: `...\05_tools\06_configs\receiver.windows.default.json`

3. 中断安装后虚拟环境“半可用”
- 仅检查 `.venv\Scripts\python.exe` 是否存在, 未校验关键依赖是否齐全.
- 首次下载被中断后, `.venv` 存在但 `av` 缺失, 导致启动即退.

4. `uv` 缓存默认路径权限问题
- 依赖补装调用 `uv pip install` 时, `uv` 尝试写入
  `C:\Users\Win10\AppData\Local\uv\cache`, 当前环境拒绝访问.

5. 进程探测依赖 `Get-CimInstance` 的脆弱性
- 某些环境下 `Get-CimInstance Win32_Process` 可能受权限策略影响.
- 仅依赖该方式查 PID 会造成“启动已成功但被误判失败”的风险.

## 3. 已完成修复

1. 新增 Windows 一键入口
- 新增 [one_click_windows.ps1](E:\Win10\Desktop\wireless_video_delivery\one_click_windows.ps1)
- 新增 [one_click_windows.cmd](E:\Win10\Desktop\wireless_video_delivery\one_click_windows.cmd)
- `cmd` 入口统一使用 `-ExecutionPolicy Bypass` 调起菜单脚本.

2. 修复根启动脚本配置路径解析
- 修改 [start_receiver_windows.ps1](E:\Win10\Desktop\wireless_video_delivery\start_receiver_windows.ps1)
- 在根脚本先把相对路径解析为绝对路径, 再传递给 `05_tools`.

3. 增强依赖自检与自动补装
- 修改 [start_receiver.ps1](E:\Win10\Desktop\wireless_video_delivery\02_receiver_windows\start_receiver.ps1)
- 新增 `numpy/cv2/av` 模块检查逻辑.
- 若缺包且允许自动安装, 自动执行 `uv pip install -r requirements.txt`.
- 安装后再次校验, 防止“表面安装成功但依赖仍缺失”.

4. 固定 `uv` 到项目本地缓存目录
- 在启动脚本中设置:
- `UV_CACHE_DIR=<project>\\.uv-cache`
- `UV_PYTHON_INSTALL_DIR=<project>\\.uv-python`
- 避免写系统全局目录导致权限错误.

5. 增加 PID 探测兜底
- 启动后如果 `Get-CimInstance` 未返回结果, 使用 `Start-Process` 返回的 `process.Id`
  二次校验并写入 `receiver.pid`.

6. 菜单输入健壮性增强
- `one_click_windows.ps1` 对菜单输入增加 `.Trim()`, 避免尾随空格导致误判.

## 4. 验证记录

已验证通过:

- `powershell -ExecutionPolicy Bypass -File .\start_receiver_windows.ps1`
  可以完成环境检查并返回 `Receiver started`.
- `@'1 3 4'@` 形式驱动 `one_click_windows.ps1` 菜单时, 可完成启动和停止流程.
- `powershell -ExecutionPolicy Bypass -File .\stop_receiver_windows.ps1 -Force`
  可清理运行态并返回 `Receiver stopped` 或 `Receiver is not running`.

当前观察:

- 在本机自动化会话中, 后台接收进程可能较快退出, `status` 可能显示 `STOPPED` 但保留最近一次
  `state=RUNNING` 日志行.
- 该行为与“无发送端输入/图形会话条件/当前终端权限策略”相关, 不属于本次一键入口与路径错误的主阻塞项.

## 5. 当前推荐用法

在项目根目录执行:

```powershell
.\one_click_windows.cmd
```

菜单操作:

- `1` 启动接收端
- `2` 查看状态
- `3` 停止接收端
- `4` 退出

如需直接命令方式:

```powershell
powershell -ExecutionPolicy Bypass -File .\start_receiver_windows.ps1
powershell -ExecutionPolicy Bypass -File .\status_receiver_windows.ps1
powershell -ExecutionPolicy Bypass -File .\stop_receiver_windows.ps1 -Force
```
