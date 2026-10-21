#!/bin/bash
# Build an experimental Navi33 Metal plug-in using the shared N48N/RADV
# userspace bridge. This packages a bundle only; it never registers, installs,
# loads, or grants hardware capabilities.
set -euo pipefail

D="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$D/../../.." && pwd)"
N48_SRC="${N33_NAVI48METAL_SOURCE:-$ROOT/tools/native/navi48metal}"
RADV="${N33_RADV_LIB:-}"
XLATE="${N33_XLATE_LIB:-}"
VK_INC="${N33_VK_INC:-}"
OUT="${N33_BRIDGE_OUTPUT_DIR:-$D/build/experimental-bridge}"

fail() { echo "build-bridge.sh: $*" >&2; exit 2; }
[ "$(uname -s)" = Darwin ] || fail "requires macOS with Xcode tools"
command -v xcrun >/dev/null || fail "xcrun is required"
command -v lipo >/dev/null || fail "lipo is required"
command -v nm >/dev/null || fail "nm is required"
command -v otool >/dev/null || fail "otool is required"
[ -d "$N48_SRC" ] || fail "shared Navi48 Metal source not found: $N48_SRC"
[ -n "$RADV" ] && [ -f "$RADV" ] || fail "set N33_RADV_LIB to the RADV dylib built from mesa-patches 0001..0011"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/navi33-metal-bridge.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# Patch 10 makes the native RADV backend choose gfx1102 from N48N QueryInfo's
# live GC 11.0.2 value. Require its CI source manifest; a filename or device
# name alone does not identify the backend generation.
MANIFEST="${N33_RADV_MANIFEST:-$(dirname "$RADV")/mesa-commits.txt}"
[ -f "$MANIFEST" ] || fail "set N33_RADV_MANIFEST to the Mesa commit manifest packaged with RADV"
grep -Fq "radv/darwin: pick the canned device from the kext's GC version" "$MANIFEST" || \
  fail "RADV manifest does not include Mesa patch 10's live Navi33 generation selection"
[ "$(lipo -archs "$RADV" 2>/dev/null)" = x86_64 ] || fail "RADV must be thin x86_64: $RADV"
nm -gU "$RADV" > "$WORK/radv.nm"
grep -Fq 'vk_icdGetInstanceProcAddr' "$WORK/radv.nm" || fail "RADV lacks vk_icdGetInstanceProcAddr"
grep -Fq 'radv_darwin_set_service_class' "$WORK/radv.nm" || fail "RADV lacks the Darwin N48N service selector"

check_system_deps() {
  local binary="$1" depsfile="$2" idfile="$3" self_id dependency
  otool -D "$binary" > "$idfile" || fail "cannot inspect dylib install ID: $binary"
  self_id="$(awk 'NR == 2 { print $1 }' "$idfile")"
  otool -L "$binary" > "$depsfile" || fail "cannot inspect dylib dependencies: $binary"
  while IFS= read -r dependency; do
    [ -z "$dependency" ] && continue
    [ -n "$self_id" ] && [ "$dependency" = "$self_id" ] && continue
    case "$dependency" in
      /usr/lib/*|/System/Library/*) ;;
      *) fail "non-system dylib dependency is not bundled: $dependency ($binary)" ;;
    esac
  done < <(awk 'NR > 1 { print $1 }' "$depsfile")
}
check_system_deps "$RADV" "$WORK/radv-deps.txt" "$WORK/radv-id.txt"

if [ -z "$XLATE" ]; then
  XLATE="$OUT/libn48xlate.dylib"
  mkdir -p "$OUT"
  N33_XLATE_OUTPUT="$XLATE" "$ROOT/third-party/metal2vulkan-n48/build-macos.sh"
fi
[ -f "$XLATE" ] || fail "translator dylib not found: $XLATE"
[ "$(lipo -archs "$XLATE" 2>/dev/null)" = x86_64 ] || fail "translator must be thin x86_64: $XLATE"
check_system_deps "$XLATE" "$WORK/xlate-deps.txt" "$WORK/xlate-id.txt"

if [ -z "$VK_INC" ]; then
  if brew --prefix vulkan-headers >/dev/null 2>&1; then
    VK_INC="-I$(brew --prefix vulkan-headers)/include"
  else
    fail "install vulkan-headers or set N33_VK_INC"
  fi
fi

ditto "$N48_SRC" "$WORK/navi48metal"
env N48_RADV_LIB="$RADV" N48_XLATE_LIB="$XLATE" N48_VK_INC="$VK_INC" N48_N33_PROFILE=1 \
  "$WORK/navi48metal/build.sh"

tar --no-same-owner -xf "$WORK/navi48metal/build/Navi48Metal.bundle.tar" -C "$WORK"
APP="$WORK/Navi48Metal.bundle"
[ -d "$APP" ] || fail "shared bridge build produced no bundle"
# Keep the loader-facing bundle name and principal class expected by the
# existing Navi48Accelerator personality; isolate its bundle identifier so
# this experimental artifact cannot be mistaken for the Navi48 product.
plutil -replace CFBundleIdentifier -string com.cobalt64.navi33.experimental-metal \
  "$APP/Contents/Info.plist"
plutil -insert Cobalt64ExperimentalBridge -bool YES "$APP/Contents/Info.plist"
codesign --force --sign - "$APP/Contents/Resources/libvulkan_radeon.dylib"
codesign --force --sign - "$APP/Contents/Resources/libn48xlate.dylib"
codesign --force --sign - "$APP"
codesign --verify --strict "$APP"

mkdir -p "$OUT"
tar --no-xattrs --no-mac-metadata -C "$(dirname "$APP")" \
  -cf "$OUT/Navi33ExperimentalMetal-x86_64.tar" "$(basename "$APP")"
shasum -a 256 "$OUT/Navi33ExperimentalMetal-x86_64.tar" \
  > "$OUT/Navi33ExperimentalMetal-x86_64.tar.sha256"
echo "built experimental bridge artifact: $OUT/Navi33ExperimentalMetal-x86_64.tar"
echo "not installed or registered; requires live Navi33 G41 nub and accelerator gate"
