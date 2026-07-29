#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODEL_ROOT="${XIAOHUAN_MODEL_ROOT:-$ROOT_DIR/models}"
MODEL_NAME="vits-melo-tts-zh_en"
ARCHIVE="$MODEL_ROOT/$MODEL_NAME.tar.bz2"
ARCHIVE_SHA256="e58351ed7149f290a54534538badd4077cdbe6fddc964b24d0bee870415d1514"
MODEL_URL="https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/$MODEL_NAME.tar.bz2"

mkdir -p "$MODEL_ROOT"

if [[ -s "$MODEL_ROOT/$MODEL_NAME/model.onnx" ]]; then
  echo "TTS model already installed: $MODEL_ROOT/$MODEL_NAME"
  exit 0
fi

curl -fL --retry 5 --retry-delay 2 -o "$ARCHIVE.part" "$MODEL_URL"
mv "$ARCHIVE.part" "$ARCHIVE"
printf '%s  %s\n' "$ARCHIVE_SHA256" "$ARCHIVE" | sha256sum --check -
tar xjf "$ARCHIVE" -C "$MODEL_ROOT"
rm -f "$ARCHIVE"

test -s "$MODEL_ROOT/$MODEL_NAME/model.onnx"
test -s "$MODEL_ROOT/$MODEL_NAME/lexicon.txt"
test -s "$MODEL_ROOT/$MODEL_NAME/tokens.txt"
echo "TTS model installed: $MODEL_ROOT/$MODEL_NAME"
