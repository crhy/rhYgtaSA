#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S plugin -B build -DCMAKE_TOOLCHAIN_FILE=plugin/toolchain-mingw32.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build
