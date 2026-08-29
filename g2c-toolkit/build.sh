#!/usr/bin/env bash
# g2c - Ghoul2 Toolkit Build (Linux / macOS)
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
OUTPUT_DIR="$PROJECT_DIR/output"
CONFIG=Release
RUN_TESTS=1

for arg in "$@"; do
    case "$arg" in
        debug)  CONFIG=Debug ;;
        clean)  rm -rf "$BUILD_DIR" "$OUTPUT_DIR"; echo "[OK] Cleaned" ;;
        notest) RUN_TESTS=0 ;;
        *) echo "Unbekanntes Argument: $arg (erlaubt: debug clean notest)"; exit 1 ;;
    esac
done

echo "============================================================"
echo " g2c - Ghoul2 Toolkit  [$CONFIG]"
echo "============================================================"

command -v cmake >/dev/null || { echo "ERROR: cmake nicht gefunden"; exit 1; }
echo "[OK] CMake $(cmake --version | head -1 | awk '{print $3}')"

cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$CONFIG" >/dev/null
echo "[OK] Konfiguriert"

cmake --build "$BUILD_DIR" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
echo "[OK] Build erfolgreich"

if [ "$RUN_TESTS" = "1" ]; then
    echo
    "$BUILD_DIR/g2_tests"
    echo "[OK] Alle Tests bestanden"
fi

mkdir -p "$OUTPUT_DIR"
cp "$BUILD_DIR/g2c" "$OUTPUT_DIR/"
echo
echo "Bereitgestellt: $OUTPUT_DIR/g2c"
