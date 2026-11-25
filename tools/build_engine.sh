#!/usr/bin/env bash
# build_engine.sh [windows|android|all] [abi ...]
#   windows  - engine libs -> engine/build-*/  (MSVC .lib on Windows,
#              system compiler .a elsewhere)
#   android  - NDK libs per ABI -> engine/build-android/<abi>/libgh_*.a
#   all      - both (default when no args)
# ABIs: arm64-v8a armeabi-v7a x86 x86_64  (default: arm64-v8a x86_64)
set -euo pipefail

WHAT="${1:-all}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENGINE="$ROOT/engine"

command -v cmake >/dev/null || { echo "[FAIL] cmake not found on PATH."; exit 1; }

build_windows() {
  echo "[*] Building engine for host..."
  if [[ "$OSTYPE" == msys* || "$OSTYPE" == cygwin* || "$OSTYPE" == win32 ]]; then
    cmake -S "$ENGINE" -B "$ENGINE/build-msvc" -G "Visual Studio 17 2022" -A x64 \
      -DGH_BUILD_RADIOTOOL=OFF
    cmake --build "$ENGINE/build-msvc" --config Release --target gh_core gh_lua gh_miniz
    echo "[OK] engine/build-msvc/Release/gh_*.lib"
  else
    cmake -S "$ENGINE" -B "$ENGINE/build" -DCMAKE_BUILD_TYPE=Release \
      -DGH_BUILD_RADIOTOOL=OFF
    cmake --build "$ENGINE/build" --target gh_core gh_lua gh_miniz -j"$(nproc 2>/dev/null || echo 4)"
    echo "[OK] engine/build/libgh_*.a"
  fi
}

build_android() {
  local ndk="${ANDROID_NDK_ROOT:-${ANDROID_NDK_HOME:-}}"
  if [[ -z "$ndk" && -n "${ANDROID_HOME:-}" ]]; then
    ndk="$(ls -d "$ANDROID_HOME"/ndk/*/ 2>/dev/null | sort -V | tail -1)"
    ndk="${ndk%/}"
  fi
  if [[ -z "$ndk" ]]; then
    echo "[FAIL] Android NDK not found. Set ANDROID_NDK_ROOT or run tools/Androidify.sh --install first."
    exit 1
  fi
  echo "[*] Using NDK: $ndk"
  local toolchain="$ndk/build/cmake/android.toolchain.cmake"
  local abis=("$@")
  [[ ${#abis[@]} -eq 0 ]] && abis=(arm64-v8a x86_64)
  for abi in "${abis[@]}"; do
    echo "[*] Building engine for Android $abi..."
    cmake -S "$ENGINE" -B "$ENGINE/build-android/$abi" -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
      -DANDROID_ABI="$abi" -DANDROID_PLATFORM=android-28 \
      -DCMAKE_BUILD_TYPE=Release -DGH_BUILD_TESTS=OFF -DGH_BUILD_RADIOTOOL=OFF
    cmake --build "$ENGINE/build-android/$abi"
  done
  echo "[OK] engine/build-android/<abi>/libgh_*.a"
}

case "$WHAT" in
  windows) build_windows ;;
  android) shift; build_android "$@" ;;
  all)     build_windows; echo; build_android "${@:2}" ;;
  *) echo "usage: $0 [windows|android|all] [abi ...]"; exit 1 ;;
esac
