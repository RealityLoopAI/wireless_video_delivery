#!/usr/bin/env bash
set -euo pipefail

cd /home/orangepi/Downloads/wireless_video_delivery
exec ./05_tools/start_sender_preview.sh "$@"
