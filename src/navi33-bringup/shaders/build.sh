#!/bin/sh
# Rebuild gradient.co (gfx1102 code object) from gradient.cl. Offline only.
# Needs clang and ld.lld with the AMDGPU target (Ubuntu LLVM 22 has it).
# tests/test_gfx11_shader.cpp checks the committed gradient.co, so CI does
# not need the toolchain; rerun this after any change to gradient.cl.
set -eu
cd "$(dirname "$0")"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
clang -x cl -cl-std=CL2.0 -target amdgcn-amd-amdhsa -mcpu=gfx1102 -nogpulib \
      -O2 -ffile-prefix-map="$PWD"=. -c gradient.cl -o "$tmp/gradient.o"
ld.lld -shared --build-id=none "$tmp/gradient.o" -o gradient.co
echo "gradient.co: $(wc -c < gradient.co) bytes, sha256 $(sha256sum gradient.co | cut -c1-16)"
