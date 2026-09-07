#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MANIFEST="$ROOT_DIR/06_configs/deployment/sdk-manifest.json"
GENERATION=""
CACHE_DIR="${GWV3_OFFLINE_CACHE:-$ROOT_DIR/11_third_party/cache}"
INSTALL_BASE="/opt/gwv3/vendor/orbbec"

usage() {
  cat <<'EOF'
usage: sudo ./05_tools/install_orbbec_sdk.sh --generation v1|v2 [options]

Options:
  --manifest PATH      SDK manifest (default: repository deployment manifest)
  --cache-dir PATH     Offline/download cache directory
  --install-base PATH  Versioned install parent (default: /opt/gwv3/vendor/orbbec)

The final line on stdout is the installed SDK root. Download progress and status
are written to stderr so callers can capture that path safely.
EOF
}

while (($#)); do
  case "$1" in
    --generation) GENERATION="${2:-}"; shift 2 ;;
    --manifest) MANIFEST="${2:-}"; shift 2 ;;
    --cache-dir) CACHE_DIR="${2:-}"; shift 2 ;;
    --install-base) INSTALL_BASE="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

fail() {
  echo "Orbbec SDK installation failed: $1" >&2
  exit 1
}

((EUID == 0)) || fail "run this installer with sudo"
[[ "$GENERATION" == "v1" || "$GENERATION" == "v2" ]] || fail "--generation must be v1 or v2"
[[ -f "$MANIFEST" ]] || fail "manifest not found: $MANIFEST"
command -v python3 >/dev/null 2>&1 || fail "python3 is required"
command -v sha256sum >/dev/null 2>&1 || fail "sha256sum is required"

mapfile -t PACKAGE < <(python3 - "$MANIFEST" "$GENERATION" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as handle:
    package = json.load(handle)["packages"][sys.argv[2]]
for key in ("version", "architecture", "archive_name", "archive_type", "url", "sha256", "sdk_root_glob"):
    value = str(package.get(key) or "")
    if not value or "\n" in value:
        raise SystemExit(f"invalid manifest field: {key}")
    print(value)
PY
)
((${#PACKAGE[@]} == 7)) || fail "manifest package is incomplete"
VERSION="${PACKAGE[0]}"
ARCHITECTURE="${PACKAGE[1]}"
ARCHIVE_NAME="${PACKAGE[2]}"
ARCHIVE_TYPE="${PACKAGE[3]}"
URL="${PACKAGE[4]}"
SHA256="${PACKAGE[5]}"
SDK_ROOT_GLOB="${PACKAGE[6]}"

case "$(uname -m)" in
  aarch64|arm64) MACHINE_ARCH=arm64 ;;
  *) MACHINE_ARCH="$(uname -m)" ;;
esac
[[ "$MACHINE_ARCH" == "$ARCHITECTURE" ]] || fail "package architecture $ARCHITECTURE does not match $MACHINE_ARCH"

install -d -m 0755 "$CACHE_DIR" "$INSTALL_BASE"
ARCHIVE="$CACHE_DIR/$ARCHIVE_NAME"
if [[ -f "$ARCHIVE" ]] && ! printf '%s  %s\n' "$SHA256" "$ARCHIVE" | sha256sum --check --status; then
  echo "cached archive checksum mismatch; replacing $ARCHIVE" >&2
  rm -f "$ARCHIVE"
fi
if [[ ! -f "$ARCHIVE" ]]; then
  command -v curl >/dev/null 2>&1 || fail "curl is required when the SDK is not in the offline cache"
  TEMP_ARCHIVE="$ARCHIVE.part.$$"
  trap 'rm -f "${TEMP_ARCHIVE:-}"; [[ -z "${TEMP_DIR:-}" ]] || rm -rf "$TEMP_DIR"' EXIT
  echo "downloading official Orbbec SDK $GENERATION $VERSION" >&2
  curl --fail --location --retry 5 --retry-delay 2 --output "$TEMP_ARCHIVE" "$URL"
  printf '%s  %s\n' "$SHA256" "$TEMP_ARCHIVE" | sha256sum --check --status \
    || fail "downloaded archive checksum mismatch"
  mv "$TEMP_ARCHIVE" "$ARCHIVE"
fi
printf '%s  %s\n' "$SHA256" "$ARCHIVE" | sha256sum --check --status \
  || fail "archive checksum mismatch"

FINAL_ROOT="$INSTALL_BASE/$GENERATION-$VERSION"
if [[ -s "$FINAL_ROOT/lib/libOrbbecSDK.so" && -d "$FINAL_ROOT/include" ]]; then
  echo "Orbbec SDK already installed: $FINAL_ROOT" >&2
  printf '%s\n' "$FINAL_ROOT"
  exit 0
fi

TEMP_DIR="$(mktemp -d "$INSTALL_BASE/.extract-${GENERATION}-${VERSION}.XXXXXX")"
trap 'rm -f "${TEMP_ARCHIVE:-}"; rm -rf "$TEMP_DIR"' EXIT
case "$ARCHIVE_TYPE" in
  zip)
    command -v unzip >/dev/null 2>&1 || fail "unzip is required for $ARCHIVE_NAME"
    unzip -q "$ARCHIVE" -d "$TEMP_DIR"
    ;;
  tar.gz)
    tar xzf "$ARCHIVE" -C "$TEMP_DIR"
    ;;
  *) fail "unsupported archive type: $ARCHIVE_TYPE" ;;
esac

mapfile -t SDK_ROOTS < <(find "$TEMP_DIR" -path "$TEMP_DIR/$SDK_ROOT_GLOB" -type d -print)
((${#SDK_ROOTS[@]} == 1)) || fail "expected one SDK root matching $SDK_ROOT_GLOB, found ${#SDK_ROOTS[@]}"
EXTRACTED_ROOT="${SDK_ROOTS[0]}"
[[ -s "$EXTRACTED_ROOT/lib/libOrbbecSDK.so" && -d "$EXTRACTED_ROOT/include" ]] \
  || fail "extracted SDK is missing include or lib/libOrbbecSDK.so"

STAGED_ROOT="$INSTALL_BASE/.install-${GENERATION}-${VERSION}.$$"
rm -rf "$STAGED_ROOT"
cp -a "$EXTRACTED_ROOT" "$STAGED_ROOT"
rm -rf "$FINAL_ROOT"
mv "$STAGED_ROOT" "$FINAL_ROOT"

UDEV_SCRIPT=""
if [[ "$GENERATION" == "v1" ]]; then
  UDEV_SCRIPT="$(find "$TEMP_DIR" -path '*/Script/install_udev_rules.sh' -type f -print -quit)"
else
  UDEV_SCRIPT="$(find "$FINAL_ROOT" -path '*/shared/install_udev_rules.sh' -type f -print -quit)"
fi
if [[ -n "$UDEV_SCRIPT" ]]; then
  bash "$UDEV_SCRIPT" >/dev/null
  udevadm control --reload-rules >/dev/null 2>&1 || true
else
  echo "warning: Orbbec udev installer was not found; camera permissions may need manual setup" >&2
fi

cat > "$FINAL_ROOT/gwv3-package.json" <<EOF
{"generation":"$GENERATION","version":"$VERSION","archive":"$ARCHIVE_NAME","sha256":"$SHA256"}
EOF
chmod 0644 "$FINAL_ROOT/gwv3-package.json"
echo "installed Orbbec SDK $GENERATION $VERSION at $FINAL_ROOT" >&2
printf '%s\n' "$FINAL_ROOT"
