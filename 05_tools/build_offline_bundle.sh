#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROLE="${1:-}"
OUTPUT="${2:-$PWD}"
[[ "$ROLE" == sender || "$ROLE" == receiver ]] || {
  echo "usage: $0 sender|receiver [output-directory]" >&2
  exit 2
}
command -v jq >/dev/null 2>&1 || { echo "jq is required" >&2; exit 1; }
command -v apt-rdepends >/dev/null 2>&1 || {
  echo "install apt-rdepends before building an offline bundle" >&2
  exit 1
}
[[ -z "$(git -C "$ROOT_DIR" status --porcelain)" ]] || {
  echo "refusing to bundle a dirty worktree; commit the release first" >&2
  exit 1
}

OS_ID="$(. /etc/os-release; printf '%s' "$ID")"
OS_VERSION="$(. /etc/os-release; printf '%s' "$VERSION_ID")"
ARCH="$(uname -m)"
if [[ "$ROLE" == sender ]]; then
  [[ "$OS_ID" == ubuntu && "$OS_VERSION" == 22.04 && "$ARCH" == aarch64 ]] || {
    echo "build Sender bundles on the supported Ubuntu 22.04 arm64 baseline" >&2; exit 1;
  }
else
  [[ "$OS_ID" == ubuntu && "$OS_VERSION" == 24.04 && "$ARCH" == x86_64 ]] || {
    echo "build Receiver bundles on the supported Ubuntu 24.04 x86_64 baseline" >&2; exit 1;
  }
fi

VERSION="$(git -C "$ROOT_DIR" describe --tags --always --dirty)"
BUNDLE_NAME="gwv3-${ROLE}-${OS_VERSION}-${ARCH}-${VERSION}"
BUNDLE="$OUTPUT/$BUNDLE_NAME"
[[ ! -e "$BUNDLE" ]] || { echo "bundle output already exists: $BUNDLE" >&2; exit 1; }
mkdir -p "$BUNDLE/cache/apt" "$BUNDLE/cache/pip"
COMMIT="$(git -C "$ROOT_DIR" rev-parse HEAD)"
git clone --quiet --depth 1 --no-local "file://$ROOT_DIR" "$BUNDLE/repository"
git -C "$BUNDLE/repository" checkout --quiet --detach "$COMMIT"
git -C "$BUNDLE/repository" remote remove origin

mapfile -t direct_packages < <(jq -r --arg role "$ROLE" \
  '.common[], .roles[$role][]' "$ROOT_DIR/06_configs/deployment/package-manifest.json")
mapfile -t packages < <(
  apt-rdepends "${direct_packages[@]}" 2>/dev/null \
    | sed -n '/^[a-zA-Z0-9][a-zA-Z0-9+.:~-]*$/p' \
    | sed 's/:any$//' \
    | sort -u
)
(
  cd "$BUNDLE/cache/apt"
  for package in "${packages[@]}"; do
    apt-get download "$package" >/dev/null 2>&1 || echo "warning: could not download $package" >&2
  done
)

python3 -m pip download --dest "$BUNDLE/cache/pip" \
  -r "$ROOT_DIR/09_web_monitor/requirements.txt"
if [[ "$ROLE" == sender ]]; then
  mapfile -t pip_packages < <(jq -r '.sender_pip[]' "$ROOT_DIR/06_configs/deployment/package-manifest.json")
  python3 -m pip download --dest "$BUNDLE/cache/pip" "${pip_packages[@]}"
  python3 - "$ROOT_DIR/06_configs/deployment/sdk-manifest.json" <<'PY' > "$BUNDLE/sdk-assets.tsv"
import json, sys
data=json.load(open(sys.argv[1], encoding="utf-8"))
for package in data["packages"].values():
    print("\t".join((package["archive_name"], package["url"], package["sha256"])))
PY
  while IFS=$'\t' read -r name url checksum; do
    curl --fail --location --retry 5 --output "$BUNDLE/cache/$name" "$url"
    printf '%s  %s\n' "$checksum" "$BUNDLE/cache/$name" | sha256sum --check --status
  done < "$BUNDLE/sdk-assets.tsv"
  rm -f "$BUNDLE/sdk-assets.tsv"
  name="vosk-model-small-cn-0.22.zip"
  url="$(jq -r '.assets.vosk_small_cn_0_22.url' "$ROOT_DIR/06_configs/deployment/asset-manifest.json")"
  checksum="$(jq -r '.assets.vosk_small_cn_0_22.sha256' "$ROOT_DIR/06_configs/deployment/asset-manifest.json")"
  curl --fail --location --retry 5 --output "$BUNDLE/cache/$name" "$url"
  printf '%s  %s\n' "$checksum" "$BUNDLE/cache/$name" | sha256sum --check --status
fi

cat > "$BUNDLE/install.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DESTINATION="$HOME/wireless_video_delivery"
if [[ -e "$DESTINATION" ]]; then
  echo "destination already exists; refusing to overwrite: $DESTINATION" >&2
  exit 1
fi
cp -a "$HERE/repository" "$DESTINATION"
exec "$DESTINATION/05_tools/bootstrap.sh" --offline-cache "$HERE/cache" "$@"
EOF
chmod 0755 "$BUNDLE/install.sh"
printf '{"role":"%s","os":"%s","os_version":"%s","arch":"%s","release":"%s"}\n' \
  "$ROLE" "$OS_ID" "$OS_VERSION" "$ARCH" "$VERSION" > "$BUNDLE/bundle.json"
tar czf "$OUTPUT/$BUNDLE_NAME.tar.gz" -C "$OUTPUT" "$BUNDLE_NAME"
echo "$OUTPUT/$BUNDLE_NAME.tar.gz"
