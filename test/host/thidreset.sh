#!/usr/bin/env bash
# Host sanitizers verify production HID lifetime with mocked USB transport.
set -euo pipefail
cd "$(dirname "$0")"
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/usbnt-hid-reset.XXXXXXXX")
trap 'rm -rf "$test_dir"' EXIT
"${CC:-gcc}" -std=c99 -O1 -g -fsanitize=address,undefined \
  -fno-omit-frame-pointer -ffunction-sections -fdata-sections \
  -DUSB_HARNESS=1 thidreset.c ../../src/hidparse.c \
  -Wl,--gc-sections -o "$test_dir/test"
"$test_dir/test"
