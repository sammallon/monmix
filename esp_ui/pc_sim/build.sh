#!/usr/bin/env bash
# Build + run the PC sim under WSL/Linux. From a WSL shell:
#   bash /mnt/s/playground/monmix/esp_ui/pc_sim/build.sh         # configure + build
#   bash /mnt/s/playground/monmix/esp_ui/pc_sim/build.sh --run    # also launches the binary
#
# One-time prerequisite (Ubuntu/Debian; needs sudo, run from WSL):
#   sudo apt install -y cmake ninja-build pkg-config libsdl2-dev
#
# WSLg ships the libX11 / libwayland runtimes already, so the resulting
# binary renders into a regular Windows window with no extra X server
# configuration.

set -euo pipefail

cd "$(dirname "$0")"

for tool in cmake ninja pkg-config gcc; do
    if ! command -v "$tool" >/dev/null; then
        echo "missing: $tool" >&2
        echo "install with: sudo apt install -y cmake ninja-build pkg-config libsdl2-dev build-essential" >&2
        exit 1
    fi
done

if ! pkg-config --exists sdl2; then
    echo "missing: libsdl2-dev" >&2
    echo "install with: sudo apt install -y libsdl2-dev" >&2
    exit 1
fi

# Per-host build dir so Windows (build-windows/) and WSL (build-linux/)
# don't fight over CMakeCache.txt — CMake records the absolute source
# path during configure and refuses to reuse a cache created with a
# different one (S:/… vs /mnt/s/…).
BUILD_DIR=build-linux
mkdir -p "$BUILD_DIR"
cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build "$BUILD_DIR"

echo "OK: $(realpath "$BUILD_DIR/monmix_sim")"

if [[ "${1:-}" == "--run" ]]; then
    exec "$BUILD_DIR/monmix_sim"
fi
