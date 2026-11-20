#!/usr/bin/env bash
# Builds the native arm64 graphics service with the arm64 video core (step 3.3b):
# out/gpu-arm64 (CMake, arm64 dependencies from packaging/macos/build-deps-arm64.sh), then
# out/service/ps4mac_service next to libspartanportgpu.dylib.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
deps=${PS4MAC_ARM64_DEPS:-$HOME/Library/Caches/ps4mac-arm64-deps}
if [[ ! -f "$deps/lib/libvulkan_kosmickrisp.dylib" ]]; then
    echo "No arm64 dependencies in $deps: run packaging/macos/build-deps-arm64.sh" >&2; exit 1
fi
if [[ ! -f out/gpu-arm64/build.ninja ]]; then
    PKG_CONFIG_PATH=$deps/lib/pkgconfig PKG_CONFIG_LIBDIR=$deps/lib/pkgconfig \
    cmake -S gpu -B out/gpu-arm64 -G Ninja -Wno-deprecated -Wno-dev -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_SYSTEM_PROCESSOR=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 \
        -DCMAKE_PREFIX_PATH="$deps;$(brew --prefix boost)" -DSPARTAN_PGO=off -DSPARTAN_LTO=OFF >/dev/null
fi
if ! ninja -j"${CMAKE_BUILD_PARALLEL_LEVEL:-4}" -C out/gpu-arm64 ps4mac_service > out/gpu-arm64-build.log 2>&1; then
    grep -v '^\[' out/gpu-arm64-build.log | tail -40 >&2
    echo 'Service build failed (full log: out/gpu-arm64-build.log)' >&2; exit 1
fi
mkdir -p out/service/vulkan
cp out/gpu-arm64/ps4mac_service out/gpu-arm64/libspartanportgpu.dylib out/service/
# The ICD file names its library relative to itself: the copy names it by its full path.
sed "s|\"library_path\": *\"[^\"]*\"|\"library_path\": \"$deps/lib/libvulkan_kosmickrisp.dylib\"|" \
    "$deps/share/vulkan/icd.d/kosmickrisp_mesa_icd.json" > out/service/vulkan/kosmickrisp_mesa_icd.json
echo out/service/ps4mac_service
