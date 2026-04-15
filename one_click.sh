#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "===== Wireless Video 一键菜单 ====="
echo "1) 启动 Linux 接收端"
echo "2) 启动 发送端（会提示输入接收端IP）"
echo "3) 查看状态"
echo "4) 停止全部"
echo "5) 退出"
read -r -p "请选择 [1-5]: " choice

case "${choice}" in
  1)
    bash "${ROOT_DIR}/start_receiver_linux.sh"
    ;;
  2)
    bash "${ROOT_DIR}/start_sender.sh"
    ;;
  3)
    bash "${ROOT_DIR}/status.sh"
    ;;
  4)
    bash "${ROOT_DIR}/stop.sh"
    ;;
  5)
    exit 0
    ;;
  *)
    echo "无效选择"
    exit 2
    ;;
esac
