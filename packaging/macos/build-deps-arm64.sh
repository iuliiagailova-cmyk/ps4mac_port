#!/usr/bin/env bash
# ps4mac_port (step 3.1): native arm64 dependencies of the graphics service.
# The game and the runtime stay x86-64 under Rosetta 2 (build-deps.sh); the graphics service runs
# natively and needs its own copies: Vulkan headers and loader, KosmicKrisp (with this repository's
# patches), SDL3, fmt, xxHash, robin-map and the VMA header. No FFmpeg or Zydis: video decoding
# and x86 code analysis stay on the guest side.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
prefix=${PS4MAC_ARM64_DEPS:-$HOME/Library/Caches/ps4mac-arm64-deps}
x86_src=${SPARTAN_MACOS_DEPS:-$HOME/Library/Caches/spartanport-macos-deps}/src
src=$prefix/src
mkdir -p "$src"
jobs=${CMAKE_BUILD_PARALLEL_LEVEL:-$(sysctl -n hw.ncpu)}

VULKAN=${VULKAN:-vulkan-sdk-1.4.357.0}
KOSMICKRISP=${KOSMICKRISP:-af7dd0a35c071cecbfff32662675641d19e8e4db}
SDL=${SDL:-3.4.16}
FMT=${FMT:-11.2.0}
XXHASH=${XXHASH:-v0.8.3}
ROBINMAP=${ROBINMAP:-v1.4.0}
VMA=${VMA:-v3.1.0}

[[ $(uname -m) == arm64 ]] || { echo 'Apple Silicon is required.' >&2; exit 1; }
for formula in cmake ninja glslang pkgconf llvm spirv-tools spirv-llvm-translator; do
    brew list --formula "$formula" >/dev/null 2>&1 || brew install "$formula"
done

cmake_arm64() { # source dir, build dir, extra args...
    local source=$1 build=$2; shift 2
    cmake -S "$source" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_PREFIX_PATH="$prefix" \
        -DCMAKE_INSTALL_NAME_DIR="$prefix/lib" "$@" >/dev/null
    ninja -j"$jobs" -C "$build" >/dev/null
    ninja -j"$jobs" -C "$build" install >/dev/null
}
# Sources: a copy of the x86 build's checkout when there is one (no download), else a fresh clone.
fetch_git() { # url, tag, dir
    [[ -d $src/$3 ]] && return
    if [[ -d $x86_src/$3/.git ]]; then
        rsync -a --exclude build --exclude 'build-*' "$x86_src/$3/" "$src/$3/"
    else
        git clone -q --depth 1 --recurse-submodules --shallow-submodules -b "$2" "$1" "$src/$3"
    fi
}
step() { [[ -f $prefix/.done-$1 ]]; }
done_step() { touch "$prefix/.done-$1"; echo "  $1"; }

echo "arm64 dependencies in $prefix"
if ! step vulkan; then
    fetch_git https://github.com/KhronosGroup/Vulkan-Headers "$VULKAN" vulkan-headers
    cmake_arm64 "$src/vulkan-headers" "$src/vulkan-headers/build"
    fetch_git https://github.com/KhronosGroup/Vulkan-Loader "$VULKAN" vulkan-loader
    cmake_arm64 "$src/vulkan-loader" "$src/vulkan-loader/build" -DUPDATE_DEPS=OFF -DBUILD_TESTS=OFF
    done_step vulkan
fi
if ! step kosmickrisp; then
    [[ -x $prefix/venv/bin/meson ]] || {
        python3 -m venv "$prefix/venv"
        "$prefix/venv/bin/pip" install -q meson mako packaging pyyaml
    }
    if [[ ! -d $src/mesa-kosmickrisp ]]; then
        if [[ -d $x86_src/mesa-kosmickrisp/externals/mesa/.git || -f $x86_src/mesa-kosmickrisp/externals/mesa/.git ]]; then
            rsync -a --exclude build "$x86_src/mesa-kosmickrisp/" "$src/mesa-kosmickrisp/"
            # The x86 tree carries its local patches and experiments: back to the pinned sources.
            git -C "$src/mesa-kosmickrisp/externals/mesa" checkout -q -- .
            git -C "$src/mesa-kosmickrisp/externals/mesa" clean -qfd
        else
            git init -q "$src/mesa-kosmickrisp"
            git -C "$src/mesa-kosmickrisp" fetch -q --depth 1 https://github.com/shadexternals/mesa-kosmickrisp "$KOSMICKRISP"
            git -C "$src/mesa-kosmickrisp" checkout -q FETCH_HEAD
            git -C "$src/mesa-kosmickrisp" submodule update -q --init --depth 1
        fi
    fi
    mesa=$src/mesa-kosmickrisp/externals/mesa
    for patch in kosmickrisp-spartanport.patch kosmickrisp-msl-packed.patch; do
        if ! git -C "$mesa" apply --reverse --check "$PWD/packaging/macos/$patch" 2>/dev/null; then
            git -C "$mesa" apply "$PWD/packaging/macos/$patch"
        fi
    done
    # No -fno-split-cold-code here: that flag works around `rep movsb` under Rosetta 2 only.
    PATH="$prefix/venv/bin:$PATH" cmake -S "$src/mesa-kosmickrisp" -B "$src/mesa-kosmickrisp/build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 >/dev/null
    PATH="$prefix/venv/bin:$PATH" ninja -j"$jobs" -C "$src/mesa-kosmickrisp/build" >/dev/null
    mkdir -p "$prefix/lib" "$prefix/share/vulkan/icd.d"
    cp "$src/mesa-kosmickrisp/build/outputs/libvulkan_kosmickrisp.dylib" "$prefix/lib/"
    sed 's|"library_path": *"[^"]*"|"library_path": "../../../lib/libvulkan_kosmickrisp.dylib"|' \
        "$src/mesa-kosmickrisp/build/outputs/kosmickrisp_mesa_icd.json" > "$prefix/share/vulkan/icd.d/kosmickrisp_mesa_icd.json"
    done_step kosmickrisp
fi
if ! step sdl3; then
    [[ -d $src/SDL3-$SDL ]] || {
        if [[ -d $x86_src/SDL3-$SDL ]]; then rsync -a --exclude build "$x86_src/SDL3-$SDL/" "$src/SDL3-$SDL/"
        else curl -fsSL "https://github.com/libsdl-org/SDL/releases/download/release-$SDL/SDL3-$SDL.tar.gz" | tar xz -C "$src"; fi
    }
    cmake_arm64 "$src/SDL3-$SDL" "$src/SDL3-$SDL/build" -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_SHARED=ON -DSDL_STATIC=OFF
    done_step sdl3
fi
if ! step fmt; then
    fetch_git https://github.com/fmtlib/fmt "$FMT" fmt
    cmake_arm64 "$src/fmt" "$src/fmt/build" -DFMT_TEST=OFF -DFMT_DOC=OFF -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    done_step fmt
fi
if ! step xxhash; then
    fetch_git https://github.com/Cyan4973/xxHash "$XXHASH" xxhash
    cmake_arm64 "$src/xxhash/cmake_unofficial" "$src/xxhash/build" -DXXHASH_BUILD_XXHSUM=OFF -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    done_step xxhash
fi
if ! step robin-map; then
    fetch_git https://github.com/Tessil/robin-map "$ROBINMAP" robin-map
    cmake_arm64 "$src/robin-map" "$src/robin-map/build"
    done_step robin-map
fi
if ! step vma; then
    mkdir -p "$prefix/include"
    if [[ -f $x86_src/../include/vk_mem_alloc.h ]]; then cp "$x86_src/../include/vk_mem_alloc.h" "$prefix/include/"
    else curl -fsSL -o "$prefix/include/vk_mem_alloc.h" \
        "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/$VMA/include/vk_mem_alloc.h"; fi
    done_step vma
fi
echo "Done: arm64 dependencies in $prefix."
