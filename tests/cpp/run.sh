#!/usr/bin/env bash
# Build & run the host-side io-homecontrol protocol golden-vector tests.
# macOS only (uses CommonCrypto as the AES oracle). No ESPHome toolchain needed.
set -euo pipefail
cd "$(dirname "$0")"

OUT="$(mktemp -d)/iohc_test"
clang++ -std=c++17 -O1 -Wall -Wextra \
  test_iohc_protocol.cpp ../../components/somfy/iohc_protocol.cpp \
  -o "$OUT"
"$OUT"
