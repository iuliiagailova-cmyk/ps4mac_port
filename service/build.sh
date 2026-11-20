#!/usr/bin/env bash
# Builds the native arm64 graphics service (step 3.2 skeleton) into out/service/ps4mac_service.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
mkdir -p out/service
clang -std=c17 -O2 -Wall -arch arm64 -mmacosx-version-min=26.0 service/ps4mac_service.c -o out/service/ps4mac_service
echo out/service/ps4mac_service
