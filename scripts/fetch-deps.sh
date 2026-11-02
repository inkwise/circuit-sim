#!/usr/bin/env bash
# Populate third_party/ from the local Termux caches when available, otherwise
# download pinned upstream releases. Idempotent: existing sources are kept.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOME_DIR="${HOME:-/data/data/com.termux/files/home}"
MATERIALIZR="$HOME_DIR/project/materializr-main"
DOWNLOADS="$ROOT/third_party/downloads"
mkdir -p "$ROOT/third_party" "$DOWNLOADS" \
    "$ROOT/android/app/src/main/java" \
    "$ROOT/android/app/src/main/assets/fonts" \
    "$ROOT/android/gradle/wrapper"

fetch() {
    local url="$1"
    local dest="$2"
    if [[ ! -f "$dest" ]]; then
        echo "-- download $(basename "$dest")"
        curl -L --fail --retry 3 --max-time 300 -o "$dest" "$url"
    fi
}

# SDL3 source
if [[ ! -f "$ROOT/third_party/SDL3/CMakeLists.txt" ]]; then
    if [[ -f "$HOME_DIR/Android/src/SDL3-3.4.18/CMakeLists.txt" ]]; then
        echo "-- copy local SDL3 3.4.18"
        mkdir -p "$ROOT/third_party/SDL3"
        cp -a "$HOME_DIR/Android/src/SDL3-3.4.18/." "$ROOT/third_party/SDL3/"
    else
        echo "-- download SDL3 3.4.18"
        fetch "https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.4.18.tar.gz" \
            "$DOWNLOADS/SDL3-3.4.18.tar.gz"
        rm -rf "$ROOT/third_party/SDL3"
        mkdir -p "$ROOT/third_party/SDL3"
        tar -xzf "$DOWNLOADS/SDL3-3.4.18.tar.gz" -C "$ROOT/third_party/SDL3" --strip-components=1
    fi
fi

# Dear ImGui source
if [[ ! -f "$ROOT/third_party/imgui/imgui.cpp" ]]; then
    if [[ -f "$HOME_DIR/Android/src/imgui/imgui.cpp" ]]; then
        echo "-- copy local Dear ImGui"
        mkdir -p "$ROOT/third_party/imgui"
        cp -a "$HOME_DIR/Android/src/imgui/." "$ROOT/third_party/imgui/"
    else
        echo "-- download Dear ImGui 1.92.8-docking"
        fetch "https://github.com/ocornut/imgui/archive/refs/tags/v1.92.8-docking.tar.gz" \
            "$DOWNLOADS/imgui-1.92.8-docking.tar.gz"
        rm -rf "$ROOT/third_party/imgui"
        mkdir -p "$ROOT/third_party/imgui"
        tar -xzf "$DOWNLOADS/imgui-1.92.8-docking.tar.gz" \
            -C "$ROOT/third_party/imgui" --strip-components=1
    fi
fi

# ngspice 47 release tarball. The SourceForge release archive includes the
# generated configure script and parser sources, so bison/flex/autotools are
# not needed on the Android build host.
if [[ ! -x "$ROOT/third_party/ngspice-47/configure" ]]; then
    echo "-- download ngspice 47 (SourceForge release)"
    NGSPICE_TAR="$DOWNLOADS/ngspice-47-sf.tar.gz"
    fetch "https://downloads.sourceforge.net/project/ngspice/ng-spice-rework/47/ngspice-47.tar.gz" \
        "$NGSPICE_TAR"
    echo "894e649651f1838a14095e5a5439e7d3aa63e87ede14d283173fda4fcdef675f  $NGSPICE_TAR" | sha256sum -c -
    rm -rf "$ROOT/third_party/ngspice-47"
    mkdir -p "$ROOT/third_party/ngspice-47"
    tar -xzf "$NGSPICE_TAR" -C "$ROOT/third_party/ngspice-47" --strip-components=1
fi

# SDL3 Android Java half.
if [[ ! -f "$ROOT/android/app/src/main/java/org/libsdl/app/SDLActivity.java" ]]; then
    echo "-- copy SDL3 Android Java classes"
    mkdir -p "$ROOT/android/app/src/main/java"
    cp -a "$ROOT/third_party/SDL3/android-project/app/src/main/java/org" \
        "$ROOT/android/app/src/main/java/"
fi

# Gradle wrapper (Gradle 8.7 is cached locally from the existing Android build).
if [[ ! -f "$ROOT/android/gradle/wrapper/gradle-wrapper.jar" ]]; then
    if [[ -f "$MATERIALIZR/android/gradle/wrapper/gradle-wrapper.jar" ]]; then
        cp "$MATERIALIZR/android/gradlew" "$ROOT/android/gradlew"
        cp "$MATERIALIZR/android/gradlew.bat" "$ROOT/android/gradlew.bat"
        cp "$MATERIALIZR/android/gradle/wrapper/gradle-wrapper.jar" \
            "$ROOT/android/gradle/wrapper/"
        cp "$MATERIALIZR/android/gradle/wrapper/gradle-wrapper.properties" \
            "$ROOT/android/gradle/wrapper/"
    elif [[ -f "$ROOT/third_party/SDL3/android-project/gradle/wrapper/gradle-wrapper.jar" ]]; then
        cp "$ROOT/third_party/SDL3/android-project/gradlew" "$ROOT/android/gradlew"
        cp "$ROOT/third_party/SDL3/android-project/gradlew.bat" "$ROOT/android/gradlew.bat"
        cp "$ROOT/third_party/SDL3/android-project/gradle/wrapper/gradle-wrapper.jar" \
            "$ROOT/android/gradle/wrapper/"
        cp "$ROOT/third_party/SDL3/android-project/gradle/wrapper/gradle-wrapper.properties" \
            "$ROOT/android/gradle/wrapper/"
    else
        echo "ERROR: no Gradle wrapper source available" >&2
        exit 1
    fi
fi
chmod +x "$ROOT/android/gradlew" 2>/dev/null || true

# Bundled OFL Chinese font subset.
FONT_DST="$ROOT/android/app/src/main/assets/fonts/CircuitLabSansSC.ttf"
if [[ ! -f "$FONT_DST" ]]; then
    if [[ -f "$MATERIALIZR/assets/fonts/MaterializrSansSC-UI.ttf" ]]; then
        cp "$MATERIALIZR/assets/fonts/MaterializrSansSC-UI.ttf" "$FONT_DST"
        cp "$MATERIALIZR/assets/fonts/licenses/NotoSansSC-OFL.txt" \
            "$ROOT/android/app/src/main/assets/fonts/OFL-NotoSansSC.txt"
        cp "$MATERIALIZR/assets/fonts/licenses/OFL-1.1.txt" \
            "$ROOT/android/app/src/main/assets/fonts/OFL-1.1.txt"
    else
        echo "ERROR: bundled CJK font missing. Provide a Noto Sans SC subset at" >&2
        echo "       $FONT_DST (OFL license)." >&2
        exit 1
    fi
fi

# Machine-local Android SDK paths.
if [[ ! -f "$ROOT/android/local.properties" ]]; then
    SDK_DIR="${ANDROID_HOME:-$HOME_DIR/Android/Sdk}"
    CMAKE_DIR="$HOME_DIR/Android/cmake"
    printf 'sdk.dir=%s\n' "$SDK_DIR" > "$ROOT/android/local.properties"
    if [[ -d "$CMAKE_DIR" ]]; then
        printf 'cmake.dir=%s\n' "$CMAKE_DIR" >> "$ROOT/android/local.properties"
    fi
fi

echo "Dependencies are ready."
echo "Next: bash scripts/build-ngspice-android.sh"
