#!/usr/bin/env bash
set -euo pipefail

if (($# != 2)); then
  echo "usage: $0 CLEAN_GSTREAMER_ROCKCHIP_SOURCE BUILD_DIRECTORY" >&2
  exit 2
fi
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="$(realpath -- "$1")"
build_dir="$(realpath -m -- "$2")"
expected=ca829e0dc1d814df01711a5796e7510812af9e85
patch_file="$root/11_third_party/patches/gstreamer-rockchip-sync-point.patch"
if [[ "$(git -C "$source_dir" rev-parse HEAD)" != "$expected" ]]; then
  echo "unsupported source commit; expected $expected" >&2
  exit 1
fi
if [[ -n "$(git -C "$source_dir" status --porcelain)" ]]; then
  echo "source is not clean; refusing to overwrite existing changes" >&2
  exit 1
fi
for tool in meson ninja pkg-config; do command -v "$tool" >/dev/null; done
pkg-config --exists gstreamer-video-1.0 gstreamer-allocators-1.0 gstreamer-pbutils-1.0 rockchip_mpp librga
git -C "$source_dir" apply --check "$patch_file"
git -C "$source_dir" apply "$patch_file"
meson setup "$build_dir" "$source_dir" --buildtype=release \
  -Drockchipmpp=enabled -Drkximage=disabled -Drga=enabled -Dglib-asserts=disabled
nice -n 15 ninja -C "$build_dir" -j1
sha256sum "$build_dir/gst/rockchipmpp/libgstrockchipmpp.so"
echo "Build only: system libraries and running services were not modified."
