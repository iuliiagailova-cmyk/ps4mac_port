#!/usr/bin/env bash
# Builds and runs the arm64 graphics service smoke test (step 3.1): window + Vulkan on KosmicKrisp.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
deps=${PS4MAC_ARM64_DEPS:-$HOME/Library/Caches/ps4mac-arm64-deps}
mkdir -p out/service
clang++ -std=c++20 -O2 -arch arm64 -I"$deps/include" service/smoke/service_smoke.cpp -L"$deps/lib" \
    -lSDL3 -lvulkan -Wl,-rpath,"$deps/lib" -o out/service/service_smoke
VK_DRIVER_FILES="$deps/share/vulkan/icd.d/kosmickrisp_mesa_icd.json" out/service/service_smoke "${1:-3}"
