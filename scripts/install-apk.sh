#!/usr/bin/env bash
# Hand the debug APK to Android's package installer from Termux.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APK="$ROOT/dist/CircuitLab-0.1.0-debug.apk"

if [[ ! -f "$APK" ]]; then
    echo "APK not found: $APK" >&2
    echo "Run scripts/build-apk.sh first." >&2
    exit 1
fi

termux-open --content-type application/vnd.android.package-archive "$APK"
