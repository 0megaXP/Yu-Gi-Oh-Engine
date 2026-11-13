#!/usr/bin/env bash
# Builds what a Windows desktop app needs from the engine, on Linux (cross-compiled):
#   <out>/ygoe_jni.dll   the native engine (needs only KERNEL32.dll and msvcrt.dll)
#   <out>/ygoe/          the asset pack (card data + scripts) to pass to YgoEngine.open
# Needs: git, python3, cmake, a JDK (for jni.h) and MinGW-w64 (g++-mingw-w64-x86-64-posix).
# usage: tools/build_windows.sh <out dir>
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="$(mkdir -p "$1" && cd "$1" && pwd)"

tools/bootstrap.sh
python3 tools/make_pack.py third_party/CardScripts data "$OUT/ygoe"

JDK_HOME="${JAVA_HOME:-$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")}"
cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=tools/cmake/mingw-w64.cmake -DCMAKE_BUILD_TYPE=Release \
  -DYGOE_BUILD_JNI=ON -DYGOE_BUILD_CLI=OFF "-DYGOE_JNI_INCLUDE_DIRS=$PWD/engine/jni/win32;$JDK_HOME/include"
cmake --build build-win -j"$(nproc)" --target ygoe_jni
cp build-win/ygoe_jni.dll "$OUT/"
echo "Windows build ready in $OUT"
