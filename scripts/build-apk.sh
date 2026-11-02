#!/usr/bin/env bash
# Build the debug APK and stage it under dist/.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ANDROID="$ROOT/android"
APK="$ANDROID/app/build/outputs/apk/debug/app-debug.apk"
VERSION="0.2.0"

if [[ ! -f "$ROOT/android/app/src/main/jniLibs/arm64-v8a/libngspice.so" ]]; then
    echo "ERROR: libngspice.so is missing; run scripts/build-ngspice-android.sh first" >&2
    exit 1
fi

cd "$ANDROID"
if ! bash ./gradlew assembleDebug --offline; then
    echo "-- offline build failed, retrying with network access"
    bash ./gradlew assembleDebug
fi

if [[ ! -f "$APK" ]]; then
    echo "ERROR: APK was not produced at $APK" >&2
    exit 1
fi

mkdir -p "$ROOT/dist"
cp "$APK" "$ROOT/dist/CircuitLab-$VERSION-debug.apk"
ls -lh "$ROOT/dist/CircuitLab-$VERSION-debug.apk"
echo "APK: $ROOT/dist/CircuitLab-$VERSION-debug.apk"
