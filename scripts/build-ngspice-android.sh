#!/usr/bin/env bash
# Cross-compile ngspice 47 as libngspice.so for Android arm64.
#
# The recipe is adapted from the proven SpiceFlow ngspice-47 Android build
# (GPL-compatible build knowledge; ngspice itself is Modified BSD).
#
# Requirements: Android NDK r29 (or compatible), a release tarball at
# third_party/ngspice-47 (SourceForge release tarball, which ships configure
# and pre-generated parser sources), patchelf, and a host gcc/clang for the
# native cmpp code-model preprocessor.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="${NGSPICE_SRC:-$ROOT/third_party/ngspice-47}"
NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-$HOME/Android/Sdk/ndk/29.0.14206865}}"
API="${ANDROID_API:-26}"
ABI="${ABI:-arm64-v8a}"
TRIPLE="${TRIPLE:-aarch64-linux-android}"
JOBS="${JOBS:-4}"
BUILD="${NGSPICE_BUILD_DIR:-$ROOT/build/ngspice-$ABI}"
DEST="$ROOT/android/app/src/main/jniLibs/$ABI"

if [[ ! -x "$SRC/configure" ]]; then
    echo "ERROR: ngspice release source not found at $SRC" >&2
    echo "       Run scripts/fetch-deps.sh first (SourceForge release tarball)." >&2
    exit 1
fi
if [[ ! -d "$NDK" ]]; then
    echo "ERROR: Android NDK not found at $NDK" >&2
    exit 1
fi

# The NDK in this Termux setup has a native-aarch64 clang under a path whose
# last component is still named linux-x86_64. Prefer the linux-aarch64 alias if
# present, but fall back to linux-x86_64.
HOST_TAG="linux-aarch64"
if [[ ! -d "$NDK/toolchains/llvm/prebuilt/$HOST_TAG" ]]; then
    HOST_TAG="linux-x86_64"
fi
TOOL="$NDK/toolchains/llvm/prebuilt/$HOST_TAG/bin"
CC="$TOOL/${TRIPLE}${API}-clang"
CXX="$TOOL/${TRIPLE}${API}-clang++"
AR="$TOOL/llvm-ar"
RANLIB="$TOOL/llvm-ranlib"
STRIP="$TOOL/llvm-strip"
READELF="$TOOL/llvm-readelf"
NM="$TOOL/llvm-nm"

for f in "$CC" "$CXX" "$AR" "$RANLIB" "$STRIP"; do
    if [[ ! -x "$f" ]]; then
        echo "ERROR: missing NDK tool: $f" >&2
        exit 1
    fi
done

COMMON=(
    --with-ngshared
    --disable-static
    --disable-osdi
    --disable-klu
    --disable-openmp
    --without-x
    --with-readline=no
    --with-editline=no
    --with-fftw3=no
    --disable-dependency-tracking
    --disable-maintainer-mode
)

FLAGS="-O2 -fPIC -Wno-error=implicit-function-declaration -Wno-error=implicit-int -Wno-error=builtin-declaration-mismatch"
LINKFLAGS="-Wl,-z,max-page-size=16384"

mkdir -p "$BUILD" "$DEST"

echo "== ngspice Android build =="
echo "source : $SRC"
echo "build  : $BUILD"
echo "output : $DEST/libngspice.so"
echo "tool   : $CC"

if [[ ! -f "$BUILD/Makefile" ]]; then
    echo "-- configure"
    (
        cd "$BUILD"
        env \
            CC="$CC" CXX="$CXX" AR="$AR" RANLIB="$RANLIB" STRIP="$STRIP" \
            CFLAGS="$FLAGS" CXXFLAGS="$FLAGS" LDFLAGS="$LINKFLAGS" \
            CC_FOR_BUILD=gcc CFLAGS_FOR_BUILD="-O2" LDFLAGS_FOR_BUILD= \
            ac_cv_lib_pthread_pthread_create=yes \
            PTHREAD_LIBS= \
            ac_cv_func_dup2=yes \
            ac_cv_func_getopt_long=yes \
            ac_cv_func_clock_gettime=yes \
            ac_cv_func_gettimeofday=yes \
            "$SRC/configure" "${COMMON[@]}" \
            --host="$TRIPLE" --build=aarch64-pc-linux-gnu \
            > configure.log 2>&1
    ) || {
        echo "ERROR: configure failed; tail of configure.log:" >&2
        tail -n 100 "$BUILD/configure.log" >&2
        exit 1
    }
fi

# Cross builds need a native cmpp generator. Keep the Android-target cmpp out
# of the default target and build build/cmpp with CC_FOR_BUILD instead.
if [[ -f "$BUILD/src/xspice/cmpp/Makefile" ]]; then
    sed -i 's/^noinst_PROGRAMS = cmpp.*/noinst_PROGRAMS =/' "$BUILD/src/xspice/cmpp/Makefile"
fi
if [[ -f "$BUILD/src/xspice/Makefile" ]]; then
    sed -i 's/^SUBDIRS = mif cm enh evt idn cmpp icm verilog vhdl$/SUBDIRS = mif cm enh evt idn cmpp icm/' "$BUILD/src/xspice/Makefile"
fi

# bionic already provides pthreads; -lpthread does not exist in the NDK, and a
# stale -lpthread poisons link tests and the final link.
while IFS= read -r -d '' makefile; do
    sed -i 's/-lpthread//g' "$makefile"
    sed -i 's/ -lstdc++//g' "$makefile"
done < <(command find "$BUILD" -type f -name Makefile -print0)

echo "-- build native cmpp"
if [[ ! -x "$BUILD/src/xspice/cmpp/build/cmpp" ]]; then
    (cd "$BUILD/src/xspice/cmpp" && make build/cmpp) || {
        echo "ERROR: native cmpp build failed" >&2
        exit 1
    }
fi

echo "-- make -j$JOBS (this takes a while)"
make -C "$BUILD" -j"$JOBS" > "$BUILD/build.log" 2>&1 || {
    echo "ERROR: ngspice make failed; tail of build.log:" >&2
    tail -n 120 "$BUILD/build.log" >&2
    exit 1
}

NGSO="$(command find "$BUILD/src/.libs" -maxdepth 1 -type f -name 'libngspice.so*' | head -n 1)"
if [[ -z "$NGSO" ]]; then
    echo "ERROR: libngspice.so was not produced" >&2
    tail -n 80 "$BUILD/build.log" >&2
    exit 1
fi

cp -L "$NGSO" "$DEST/libngspice.so"
if command -v patchelf >/dev/null 2>&1; then
    patchelf --set-soname libngspice.so "$DEST/libngspice.so"
else
    echo "WARNING: patchelf not found; libngspice SONAME may be versioned" >&2
fi
"$STRIP" --strip-unneeded "$DEST/libngspice.so"

echo "-- verify"
"$READELF" -d "$DEST/libngspice.so" | command grep -E 'SONAME|NEEDED' || true
if ! "$NM" -D --defined-only "$DEST/libngspice.so" | command grep -q ' ngSpice_Init$'; then
    echo "ERROR: ngSpice_Init is not exported" >&2
    exit 1
fi
if "$READELF" -d "$DEST/libngspice.so" | command grep -q 'libstdc++'; then
    echo "ERROR: libngspice still depends on libstdc++" >&2
    exit 1
fi
"$READELF" -l "$DEST/libngspice.so" | command grep 'LOAD' | head -3 || true
echo "OK: $DEST/libngspice.so"
