#!/usr/bin/env bash
# Build the stage_ui PC simulator on Linux / WSL. Mirrors
# esp_ui/pc_sim/build.sh.
set -euo pipefail

# Require SDL2 dev headers (system pkg-config path). On Ubuntu/Debian:
if ! pkg-config --exists sdl2; then
    cat >&2 <<EOF
sdl2 not found via pkg-config. Install with one of:
  sudo apt install libsdl2-dev pkg-config build-essential ninja-build cmake
  sudo dnf install SDL2-devel pkgconf-pkg-config ninja-build cmake
Then re-run this script.
EOF
    exit 1
fi

scriptDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
buildDir="$scriptDir/build-linux"
mkdir -p "$buildDir"

cmake -S "$scriptDir" -B "$buildDir" -G Ninja \
      -DCMAKE_BUILD_TYPE=Debug
cmake --build "$buildDir" --config Debug

echo "OK: $buildDir/stage_ui_sim"
case " $* " in
    *" --run "*) "$buildDir/stage_ui_sim" ;;
esac
