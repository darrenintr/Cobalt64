#!/bin/bash
# Build the vendored n48xlate adapter against its pinned upstream base.
# This produces a translator dylib only; the base project and build tree live
# outside the repository.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
OUTPUT="${N33_XLATE_OUTPUT:-$ROOT/build/libn48xlate.dylib}"
BASE_COMMIT=43c46ac8a24adf1a6e872b8a52c706ec9614fad0
UPSTREAM=https://github.com/steelbrain/metal2vulkan.git

if [ "$(uname -s)" != Darwin ]; then
  echo "build-macos.sh requires macOS and the Apple linker" >&2
  exit 2
fi
command -v cargo >/dev/null || { echo "cargo is required (Rust >= 1.87)" >&2; exit 2; }
command -v git >/dev/null || { echo "git is required" >&2; exit 2; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cobalt64-metal2vulkan-n48.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
M2V_DIR="$WORK/metal2vulkan"
git clone --quiet "$UPSTREAM" "$M2V_DIR"
git -C "$M2V_DIR" checkout --quiet --detach "$BASE_COMMIT"

PATCH="$ROOT/patches/0001-metal2vulkan-changes-for-n48xlate.patch"
if ! git -C "$M2V_DIR" apply --reverse --check "$PATCH" >/dev/null 2>&1; then
  git -C "$M2V_DIR" apply --check "$PATCH"
  git -C "$M2V_DIR" apply "$PATCH"
fi
cp -R "$ROOT/n48xlate" "$M2V_DIR/n48xlate"

(cd "$M2V_DIR" && MACOSX_DEPLOYMENT_TARGET=12.0 \
  cargo build --release -p n48xlate --target x86_64-apple-darwin)
LIB="$M2V_DIR/target/x86_64-apple-darwin/release/libn48xlate.dylib"
[ -f "$LIB" ] || { echo "translator build did not produce $LIB" >&2; exit 1; }
[ "$(lipo -archs "$LIB")" = x86_64 ] || {
  echo "translator is not a thin x86_64 dylib: $LIB" >&2; exit 1;
}
mkdir -p "$(dirname "$OUTPUT")"
cp "$LIB" "$OUTPUT"
echo "built translator: $OUTPUT"
