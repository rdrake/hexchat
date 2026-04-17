#!/bin/sh
set -eu

cd "$(git rev-parse --show-toplevel)"

if [ ! -d builddir ]; then
  meson setup builddir
fi

ninja -C builddir src/fe-apple/libhexchatappleadapter.dylib

export DYLD_LIBRARY_PATH="$(pwd)/builddir/src/fe-apple:${DYLD_LIBRARY_PATH:-}"

(cd apple/macos && xcrun swift build -c debug)
(cd apple/macos && xcrun swift run HexChatAppleSmoke)
