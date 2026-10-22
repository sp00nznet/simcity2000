#!/bin/sh
# Build the 32-bit Windows host on Linux (or macOS): clang-cl and lld-link
# against xwin's MSVC CRT and Windows SDK (cmake/clang-cl-x86.cmake). The
# same build/sc2k.exe tools\build.cmd makes, played under Wine.
#
# Needs clang, lld, llvm, cmake, ninja and xwin, and once:
#   xwin --accept-license --arch x86 splat --output ~/.xwin
# setup.sh does all of it.
set -e
cd "$(dirname "$0")"
BUILD_DIR=${BUILD_DIR:-build}
# The toolkit: PCRECOMP, else ../pcrecomp, else ../tools.
if [ -z "$PCRECOMP" ]; then
  if [ ! -d ../pcrecomp ] && [ -d ../tools/runtime ]; then PCRECOMP=$(cd ../tools && pwd)
  else PCRECOMP=$(cd .. && pwd)/pcrecomp; fi
fi
if [ ! -f "$BUILD_DIR/build.ninja" ]; then
  cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=cmake/clang-cl-x86.cmake -DPCRECOMP="$PCRECOMP"
fi
cmake --build "$BUILD_DIR" "$@"
