#!/usr/bin/env bash
# Build + run the native unit tests (Linux/WSL).
set -euo pipefail
cd "$(dirname "$0")"

mkdir -p build-linux
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-linux
(cd build-linux && ctest --output-on-failure)
