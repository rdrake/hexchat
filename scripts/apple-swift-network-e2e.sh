#!/bin/sh
set -eu

cd "$(git rev-parse --show-toplevel)"

if [ ! -d builddir ]; then
  meson setup builddir
fi

ninja -C builddir src/fe-apple/libhexchatappleadapter.dylib

export DYLD_LIBRARY_PATH="$(pwd)/builddir/src/fe-apple:${DYLD_LIBRARY_PATH:-}"
export CLANG_MODULE_CACHE_PATH="${CLANG_MODULE_CACHE_PATH:-/tmp/hexchat-clang-module-cache}"
export SWIFTPM_MODULECACHE_OVERRIDE="${SWIFTPM_MODULECACHE_OVERRIDE:-/tmp/hexchat-swiftpm-module-cache}"
mkdir -p "$CLANG_MODULE_CACHE_PATH" "$SWIFTPM_MODULECACHE_OVERRIDE"

(cd apple/macos && xcrun swift run HexChatAppleNetworkE2E)
