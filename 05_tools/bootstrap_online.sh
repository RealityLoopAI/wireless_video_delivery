#!/usr/bin/env bash
set -euo pipefail

REPOSITORY_URL="${GWV3_REPOSITORY_URL:-https://github.com/RealityLoopAI/wireless_video_delivery.git}"
RELEASE_REF="${GWV3_RELEASE_REF:-field-v2026.09.07.3}"
INSTALL_DIR="${GWV3_INSTALL_DIR:-$HOME/wireless_video_delivery}"

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  cat <<'EOF'
usage: ./bootstrap_online.sh --role sender|receiver [bootstrap options]

Environment overrides:
  GWV3_RELEASE_REF    Frozen release tag to install
  GWV3_INSTALL_DIR    New local installation directory
  GWV3_REPOSITORY_URL Git repository URL
EOF
  exit 0
fi

if [[ ! -d "$INSTALL_DIR/.git" ]]; then
  if ! command -v git >/dev/null 2>&1; then
    sudo apt-get update
    sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-upgrade git ca-certificates
  fi
  git clone --branch "$RELEASE_REF" --depth 1 "$REPOSITORY_URL" "$INSTALL_DIR"
else
  echo "Refusing to update an existing installation automatically: $INSTALL_DIR" >&2
  echo "Use the checked-out repository's 05_tools/bootstrap.sh explicitly." >&2
  exit 1
fi
exec "$INSTALL_DIR/05_tools/bootstrap.sh" "$@"
