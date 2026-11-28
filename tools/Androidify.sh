#!/usr/bin/env bash
# ============================================================
#  Androidify - build the R-Slice Android APK via Lime
#
#  Checks the toolchain (Haxe, haxelibs, JDK 17, Android SDK,
#  NDK, lime config), fixes what it can, then runs
#  "lime build android".
#
#  Usage:  ./tools/Androidify.sh [--check] [--install]
#            --check    verify only, never build or install
#            --install   download/install missing pieces
#                       (JDK 17 tarball, Android cmdline-tools,
#                        SDK packages, NDK) before building
# ============================================================
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

CHECK_ONLY=0
DO_INSTALL=0
for a in "$@"; do
  case "$a" in
    --check) CHECK_ONLY=1 ;;
    --install) DO_INSTALL=1 ;;
  esac
done

FAIL=0
WARN=0

echo "============================================================"
echo " Androidify - R-Slice APK builder"
echo "============================================================"
echo

# ---------- 1. Haxe toolchain ----------
echo "[1/6] Haxe toolchain"
HAXE_DIR="${HAXE_DIR:-}"
if [ -z "$HAXE_DIR" ]; then
  if command -v haxe >/dev/null 2>&1; then
    HAXE_DIR="$(dirname "$(command -v haxe)")"
  elif [ -x /c/HaxeToolkit/haxe/haxe.exe ]; then
    HAXE_DIR="/c/HaxeToolkit/haxe"
  elif ls -d /c/haxe/haxe_*/haxe.exe >/dev/null 2>&1; then
    HAXE_DIR="$(dirname "$(ls -d /c/haxe/haxe_*/haxe.exe | head -1)")"
  fi
fi
# HAXE_DIR may be a custom install - put it on PATH for this script
case ":$PATH:" in *":$HAXE_DIR:"*) ;; *) [ -n "$HAXE_DIR" ] && export PATH="$HAXE_DIR:$PATH";; esac
if command -v haxe >/dev/null 2>&1; then
  echo "  [ OK ] haxe $(haxe --version 2>/dev/null | head -1)"
else
  echo "  [FAIL] haxe not found on PATH, /c/HaxeToolkit/haxe, or /c/haxe/haxe_*"
  echo "         Install Haxe from https://haxe.org then re-run."
  FAIL=1
fi
command -v haxelib >/dev/null 2>&1 || {
  echo "  [FAIL] haxelib not found - it ships with Haxe"
  FAIL=1
}

# HAXE_STD_PATH: some installs can't auto-detect std
if [ -z "${HAXE_STD_PATH:-}" ] && [ -n "$HAXE_DIR" ]; then
  for cand in "$HAXE_DIR/std" "$HAXE_DIR/../std"; do
    [ -f "$cand/Any.hx" ] && export HAXE_STD_PATH="$cand"
  done
fi
if [ -n "${HAXE_STD_PATH:-}" ]; then
  echo "  [ OK ] HAXE_STD_PATH=$HAXE_STD_PATH"
else
  echo "  [WARN] HAXE_STD_PATH not set and std not auto-found"
  WARN=$((WARN+1))
fi

# ---------- 2. Project haxelibs ----------
echo
echo "[2/6] Project haxelibs (.haxelib)"
if [ -f ".haxelib/lime/git/RunScript.hx" ]; then
  echo "  [ OK ] lime + dependencies present"
else
  echo "  [FAIL] .haxelib missing - run:  hmm install"
  echo "         (install hmm first: haxelib --global install hmm)"
  FAIL=1
fi

# ---------- helpers ----------
JDK_ROOT=""
find_jdk() {
  JDK_ROOT=""
  if [ -n "${JAVA_HOME:-}" ] && [ -x "$JAVA_HOME/bin/javac" ]; then
    JDK_ROOT="$JAVA_HOME"
  elif command -v javac >/dev/null 2>&1; then
    JDK_ROOT="$(dirname "$(dirname "$(command -v javac)")")"
  else
    for d in "$ROOT"/tools/jdk-17*; do
      [ -x "$d/bin/javac" ] && JDK_ROOT="$d"
    done
  fi
  if [ -n "$JDK_ROOT" ]; then
    JAVAC_VER="$("$JDK_ROOT/bin/javac" -version 2>&1 | awk '{print $2}')"
  fi
}

install_jdk() {
  # Temurin JDK 17 tarball (no root needed) into tools/jdk-17*
  echo "  [ .. ] installing JDK 17 via Adoptium..."
  local os arch url
  case "$(uname -s)" in
    Linux)  os=linux ;;
    Darwin) os=mac ;;
    MINGW*|MSYS*|CYGWIN*) os=windows ;;
    *) echo "  [FAIL] unsupported OS for auto JDK install"; return ;;
  esac
  case "$(uname -m)" in
    x86_64|amd64) arch=x64 ;;
    aarch64|arm64) arch=aarch64 ;;
    *) arch=x64 ;;
  esac
  # The /v3/binary/latest endpoint 404s; query the assets API for the
  # actual archive link (returns .zip on Windows, .tar.gz elsewhere).
  local meta; meta="$(mktemp)"
  curl -fsSL --retry 3 -o "$meta" \
    "https://api.adoptium.net/v3/assets/latest/17/hotspot?architecture=$arch&image_type=jdk&os=$os&vendor=eclipse&heap_size=normal&jvm_impl=hotspot" || {
    echo "  [FAIL] JDK metadata fetch failed - get it manually:"
    echo "         https://adoptium.net/temurin/releases/?version=17"
    rm -f "$meta"; return
  }
  url="$(grep -oE '"link": *"[^"]*\.(zip|tar\.gz)"' "$meta" | head -1 | sed -E 's/.*"([^"]+)"$/\1/')"
  rm -f "$meta"
  if [ -z "$url" ]; then
    echo "  [FAIL] no JDK archive link in Adoptium response"
    return
  fi
  local tmp; tmp="$(mktemp -d)"
  if ! curl -fL --retry 3 -o "$tmp/jdk.archive" "$url" 2>/dev/null; then
    echo "  [FAIL] JDK download failed - get it manually:"
    echo "         https://adoptium.net/temurin/releases/?version=17"
    rm -rf "$tmp"; return
  fi
  mkdir -p "$ROOT/tools"
  case "$url" in
    *.zip) unzip -q -o "$tmp/jdk.archive" -d "$ROOT/tools" 2>/dev/null || {
      echo "  [FAIL] JDK extract failed (unzip missing?)"; rm -rf "$tmp"; return; } ;;
    *) tar -xzf "$tmp/jdk.archive" -C "$ROOT/tools" 2>/dev/null ;;
  esac
  rm -rf "$tmp"
  for d in "$ROOT"/tools/jdk-17*; do
    [ -x "$d/bin/javac" ] || [ -x "$d/Contents/Home/bin/javac" ] && JDK_ROOT="$d"
  done
  # macOS layout: jdk-17.jdk/Contents/Home
  for d in "$ROOT"/tools/jdk-17*.jdk; do
    [ -d "$d/Contents/Home" ] && JDK_ROOT="$d/Contents/Home"
  done
  [ -n "${JDK_ROOT:-}" ] && echo "  [ OK ] JDK 17 installed to $JDK_ROOT"
}

# ---------- JDK ----------
find_jdk
if [ -z "$JDK_ROOT" ] && [ "$DO_INSTALL" -eq 1 ]; then install_jdk; find_jdk; fi
JAVAC_VER="${JAVAC_VER:-}"

# ---------- Android SDK ----------
ANDROID_HOME="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}"
export ANDROID_HOME
SDKM=""
find_sdkmanager() {
  SDKM=""
  for c in \
    "$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager" \
    "$ANDROID_HOME/cmdline-tools"/*/bin/sdkmanager \
    "$ANDROID_HOME/tools/bin/sdkmanager"; do
    [ -x "$c" ] || [ -f "$c" ] && { SDKM="$c"; break; }
  done
}
find_sdkmanager

install_sdk_tools() {
  # Android command-line tools for the host OS
  echo "  [ .. ] installing Android command-line tools..."
  local os arch url tmp
  case "$(uname -s)" in
    Linux)  os=linux ;;
    Darwin) os=darwin ;;
    MINGW*|MSYS*|CYGWIN*) os=win ;;
    *) echo "  [FAIL] unsupported OS for auto SDK install"; return ;;
  esac
  case "$(uname -m)" in
    x86_64|amd64) arch=x86_64 ;;
    aarch64|arm64) arch=aarch64 ;;
    *) arch=x86_64 ;;
  esac
  url="https://dl.google.com/android/repository/commandlinetools-$os-$arch-$os.zip"
  # Google's zip names: commandlinetools-{linux,darwin,win}-{x86_64,aarch64}-<ver>_latest.zip
  # win uses 'win' + x86_64 only; darwin/linux support aarch64 on newer packages
  case "$os" in
    win) url="https://dl.google.com/android/repository/commandlinetools-win-13114758_latest.zip" ;;
    darwin) url="https://dl.google.com/android/repository/commandlinetools-darwin-13114758_latest.zip" ;;
    linux) url="https://dl.google.com/android/repository/commandlinetools-linux-13114758_latest.zip" ;;
  esac
  tmp="$(mktemp -d)"
  if ! curl -fL --retry 3 -o "$tmp/tools.zip" "$url" 2>/dev/null; then
    # fallback: a well-known older build
    case "$os" in
      win) url="https://dl.google.com/android/repository/commandlinetools-win-11076708_latest.zip" ;;
      darwin) url="https://dl.google.com/android/repository/commandlinetools-darwin-11076708_latest.zip" ;;
      linux) url="https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip" ;;
    esac
    curl -fL --retry 3 -o "$tmp/tools.zip" "$url" 2>/dev/null || {
      echo "  [FAIL] cmdline-tools download failed - install Android Studio"
      echo "         command-line tools manually, or set ANDROID_HOME."
      rm -rf "$tmp"; return
    }
  fi
  unzip -q "$tmp/tools.zip" -d "$tmp" 2>/dev/null || { echo "  [FAIL] cmdline-tools extract failed"; rm -rf "$tmp"; return; }
  mkdir -p "$ANDROID_HOME/cmdline-tools/latest"
  cp -r "$tmp/cmdline-tools/." "$ANDROID_HOME/cmdline-tools/latest/" 2>/dev/null
  chmod +x "$ANDROID_HOME"/cmdline-tools/latest/bin/* 2>/dev/null
  rm -rf "$tmp"
  echo "  [ OK ] cmdline-tools installed to $ANDROID_HOME/cmdline-tools/latest"
  find_sdkmanager
}

accept_licenses() {
  [ -z "$SDKM" ] && return
  export JAVA_HOME="${JDK_ROOT:-${JAVA_HOME:-}}"
  [ -n "$JDK_ROOT" ] && export PATH="$JDK_ROOT/bin:$PATH"
  yes | "$SDKM" --sdk_root="$ANDROID_HOME" --licenses >/dev/null 2>&1
}

install_sdk_packages() {
  [ -z "$SDKM" ] && find_sdkmanager
  [ -z "$SDKM" ] && return
  export JAVA_HOME="${JDK_ROOT:-${JAVA_HOME:-}}"
  [ -n "$JDK_ROOT" ] && export PATH="$JDK_ROOT/bin:$PATH"
  echo "  [ .. ] installing SDK packages (platforms, build-tools, NDK 29.0.13113456)..."
  "$SDKM" --sdk_root="$ANDROID_HOME" \
    "platforms;android-29" "platforms;android-35" \
    "build-tools;35.0.0" "ndk;29.0.13113456"
}

# run installs if requested
if [ "$DO_INSTALL" -eq 1 ]; then
  if [ -z "$JDK_ROOT" ]; then install_jdk; find_jdk; fi
  if [ -z "$SDKM" ]; then
    install_sdk_tools
    accept_licenses
    install_sdk_packages
    find_sdkmanager
  else
    # sdkmanager exists but packages may be missing
    accept_licenses
    install_sdk_packages
  fi
fi

# ---------- 3. JDK 17 ----------
echo
echo "[3/6] JDK 17"
if [ -n "$JDK_ROOT" ]; then
  echo "  [ OK ] JDK at $JDK_ROOT (javac ${JAVAC_VER:-?})"
else
  echo "  [FAIL] javac not found - install JDK 17 (Temurin recommended)"
  echo "         https://adoptium.net/temurin/releases/?version=17"
  echo "         or re-run with --install to fetch it automatically"
  FAIL=1
fi

# ---------- 4. Android SDK ----------
echo
echo "[4/6] Android SDK"
if [ -d "$ANDROID_HOME/platforms/android-35" ]; then
  echo "  [ OK ] ANDROID_HOME=$ANDROID_HOME"
else
  echo "  [FAIL] Android SDK not usable at $ANDROID_HOME"
  echo "         Re-run with --install to fetch cmdline-tools + packages,"
  echo "         or set ANDROID_HOME to an existing SDK."
  FAIL=1
fi

# ---------- 5. lime config ----------
echo
echo "[5/6] Lime android config"
if [ "$FAIL" -eq 0 ]; then
  NDK_DIR="$(ls -d "$ANDROID_HOME"/ndk/*/ 2>/dev/null | head -1)"
  NDK_DIR="${NDK_DIR%/}"
  if [ -n "$NDK_DIR" ]; then
    haxelib run lime config ANDROID_SDK "$ANDROID_HOME" >/dev/null 2>&1
    haxelib run lime config ANDROID_NDK_ROOT "$NDK_DIR" >/dev/null 2>&1
    [ -n "${JDK_ROOT:-${JAVA_HOME:-}}" ] && haxelib run lime config JAVA_HOME "${JDK_ROOT:-$JAVA_HOME}" >/dev/null 2>&1
    haxelib run lime config ANDROID_SETUP true >/dev/null 2>&1
    echo "  [ OK ] wrote lime config (SDK, NDK: $NDK_DIR, JAVA_HOME)"
  else
    echo "  [FAIL] no NDK found under $ANDROID_HOME/ndk"
    echo "         Install the Lime-compatible NDK:"
    echo "         sdkmanager \"ndk;29.0.13113456\" --sdk_root=\"$ANDROID_HOME\""
    FAIL=1
  fi
fi

# ---------- lime Android ndll ----------
# hmm.json installs lime from the FunkinCrew/lime git fork. Git checkouts
# ship an empty ndll/ dir AND upstream release binaries are JNI-incompatible
# with the fork, so liblime-*.so must be built from source with
# "lime rebuild android" (same as the fork's Android CI). This takes a while.
ensure_lime_ndll() {
  local lime_dir=""
  for d in "$ROOT"/.haxelib/lime/*/; do
    [ -f "$d/haxelib.json" ] && lime_dir="${d%/}" && break
  done
  [ -z "$lime_dir" ] && { echo "  [FAIL] lime haxelib not found - run hmm install first"; return 1; }
  if ls "$lime_dir"/ndll/Android/liblime-64.so >/dev/null 2>&1; then
    echo "  [ OK ] lime Android ndll present"
    return 0
  fi
  if [ -d "$lime_dir/.git" ] || [ -f "$lime_dir/.git" ]; then
    echo "  [ .. ] lime git checkout lacks Android ndll - rebuilding from source..."
    echo "         (this compiles SDL/lime for arm64-v8a + armeabi-v7a; can take a while)"
    ( cd "$ROOT" && haxelib run lime rebuild android -release ) || {
      echo "  [FAIL] lime rebuild android failed - see output above"
      return 1
    }
    if ls "$lime_dir"/ndll/Android/liblime-64.so >/dev/null 2>&1; then
      echo "  [ OK ] built lime Android ndll from pinned fork source"
      return 0
    fi
    echo "  [FAIL] lime rebuild did not produce ndll/Android/liblime-64.so"
    return 1
  fi
  echo "  [ .. ] lime install lacks Android ndll - fetching release libs..."
  local ver
  ver="$(grep -oE '"version"[^,}]*' "$lime_dir/haxelib.json" | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
  local tmp; tmp="$(mktemp -d)"
  # The git checkout's version may not exist as a release; if the download
  # fails (or no version was found), fall back to the latest release listed
  # on the lib.haxe.org project page.
  if [ -n "$ver" ] && curl -fsSL --retry 3 -o "$tmp/lime.zip" "https://lib.haxe.org/p/lime/$ver/download/" 2>/dev/null; then
    : # got it
  else
    ver="$(curl -fsSL "https://lib.haxe.org/p/lime/" 2>/dev/null | grep -oE '/p/lime/[0-9]+\.[0-9]+\.[0-9]+/download/' | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | sort -V | tail -1)"
    [ -z "$ver" ] && { echo "  [FAIL] could not determine lime release version"; rm -rf "$tmp"; return 1; }
    curl -fsSL --retry 3 -o "$tmp/lime.zip" "https://lib.haxe.org/p/lime/$ver/download/" 2>/dev/null || {
      echo "  [FAIL] could not download lime $ver release"; rm -rf "$tmp"; return 1; }
  fi
  mkdir -p "$lime_dir/ndll/Android"
  unzip -o -j "$tmp/lime.zip" "*/ndll/Android/*" -d "$lime_dir/ndll/Android" >/dev/null 2>&1 \
    || unzip -o -j "$tmp/lime.zip" "ndll/Android/*" -d "$lime_dir/ndll/Android" >/dev/null 2>&1
  rm -rf "$tmp"
  if ls "$lime_dir"/ndll/Android/liblime-64.so >/dev/null 2>&1; then
    echo "  [ OK ] installed lime Android ndll from release $ver"
  else
    echo "  [FAIL] ndll/Android missing from downloaded lime package"
    return 1
  fi
}

# ---------- 6. build ----------
echo
echo "[6/6] Build"
if [ "$FAIL" -ne 0 ]; then
  echo
  echo "============================================================"
  echo " NOT BUILDING - fix the [FAIL] items above first."
  echo " Manual fallback:  haxelib run lime setup android"
  echo "                    haxelib run lime build android"
  echo "============================================================"
  exit 1
fi
if [ "$CHECK_ONLY" -eq 1 ]; then
  echo
  echo "============================================================"
  echo " CHECK PASSED - run without --check to build the APK."
  echo "============================================================"
  exit 0
fi

ensure_lime_ndll || exit 1

echo "  Building RAID engine native libs for Android..."
"$ROOT/tools/build_engine.sh" android arm64-v8a armeabi-v7a x86_64 x86 || {
  echo "[FAIL] engine build failed - cannot produce a RAID-enabled APK."
  exit 1
}

echo "  Running: haxelib run lime build android -DNO_FEATURE_VIDEO_PLAYBACK"
export JAVA_HOME="${JDK_ROOT:-${JAVA_HOME:-}}"
[ -n "$JDK_ROOT" ] && export PATH="$JDK_ROOT/bin:$PATH"
# -DNO_FEATURE_VIDEO_PLAYBACK: hxvlc ships no Android VLC libs, so video
# playback (splash MP4, cutscenes) is disabled on Android for now.
haxelib run lime build android -DNO_FEATURE_VIDEO_PLAYBACK
RC=$?
echo
if [ "$RC" -eq 0 ]; then
  echo "============================================================"
  echo " BUILD OK"
  find export/release/android/bin -maxdepth 1 \( -name '*.apk' -o -name '*.aab' \) -print 2>/dev/null | sed 's/^/  Output: /'
  echo "============================================================"
else
  echo "============================================================"
  echo " BUILD FAILED (exit $RC) - see output above."
  echo "============================================================"
fi
exit "$RC"
