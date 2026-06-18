#!/usr/bin/env bash

gemini_sender_resolve_orbbec_sdk_root() {
  local root_dir="$1"
  if [[ -n "${ORBBEC_SDK_ROOT:-}" ]]; then
    if [[ -d "$ORBBEC_SDK_ROOT/include" && -d "$ORBBEC_SDK_ROOT/lib" && -f "$ORBBEC_SDK_ROOT/lib/libOrbbecSDK.so" ]]; then
      printf '%s\n' "$ORBBEC_SDK_ROOT"
      return 0
    fi
    return 1
  fi

  local candidates=(
    "$root_dir/11_third_party/orbbec/linux_arm64/OrbbecSDK_v2.8.6"
    "$root_dir/11_third_party/orbbec/linux_arm64/OrbbecSDK_C_C++_v1.10.27_20250925_0549823_linux_arm64_release/OrbbecSDK_v1.10.27/SDK"
  )
  local candidate
  for candidate in "${candidates[@]}"; do
    if [[ -d "$candidate/include" && -d "$candidate/lib" && -f "$candidate/lib/libOrbbecSDK.so" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  return 1
}

gemini_sender_resolve_orbbec_sdk_lib() {
  local root_dir="$1"
  local sdk_root
  sdk_root="$(gemini_sender_resolve_orbbec_sdk_root "$root_dir")" || return 1
  printf '%s\n' "$sdk_root/lib"
}

gemini_sender_resolve_orbbec_sdk_config() {
  local sdk_root="$1"
  local candidates=(
    "$sdk_root/config/OrbbecSDKConfig_v1.0.xml"
    "$sdk_root/lib/OrbbecSDKConfig.xml"
    "$sdk_root/shared/OrbbecSDKConfig.xml"
  )
  local candidate
  for candidate in "${candidates[@]}"; do
    if [[ -f "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  return 1
}
