#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROLE="${1:-}"
[[ -n "$ROLE" ]] || {
  echo "usage: $0 sender [options] | receiver [config] | nas [share]" >&2
  exit 2
}
shift

case "$ROLE" in
  sender)
    exec "$ROOT_DIR/05_tools/install_sender_service.sh" "$@"
    ;;
  receiver)
    if (( EUID == 0 )); then
      echo "run the receiver installer as its desktop user, without sudo" >&2
      exit 1
    fi
    exec "$ROOT_DIR/05_tools/install_receiver_autostart.sh" "$@"
    ;;
  nas)
    (( EUID == 0 )) || {
      echo "run the NAS installer with sudo" >&2
      exit 1
    }
    exec "$ROOT_DIR/05_tools/install_nas_discovery_beacon.sh" "$@"
    ;;
  *)
    echo "unsupported role: $ROLE" >&2
    exit 2
    ;;
esac
