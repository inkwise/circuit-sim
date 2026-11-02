#!/usr/bin/env bash
# Build the headless core/ngspice acceptance tests with the NDK and run them
# under Termux. This exercises the same libngspice.so and core sources used by
# the APK.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-$HOME/Android/Sdk/ndk/29.0.14206865}}"
ABI="arm64-v8a"
API="26"
BUILD="$ROOT/build/tests-android"
LIB_DIR="$ROOT/android/app/src/main/jniLibs/$ABI"
HOST_TAG="linux-aarch64"
if [[ ! -d "$NDK/toolchains/llvm/prebuilt/$HOST_TAG" ]]; then
    HOST_TAG="linux-x86_64"
fi
SYSROOT="$NDK/toolchains/llvm/prebuilt/$HOST_TAG/sysroot/usr/lib/aarch64-linux-android"

if [[ ! -f "$LIB_DIR/libngspice.so" ]]; then
    echo "ERROR: $LIB_DIR/libngspice.so is missing; run scripts/build-ngspice-android.sh" >&2
    exit 1
fi

cmake -S "$ROOT/tests" -B "$BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ABI" \
    -DANDROID_PLATFORM="android-$API" \
    -DANDROID_STL=c++_shared \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" -j"${JOBS:-4}"

echo "---- running engine tests ----"
LD_LIBRARY_PATH="$LIB_DIR:$SYSROOT" "$BUILD/engine_tests"
