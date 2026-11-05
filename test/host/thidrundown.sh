#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/usbnt-hid-rundown.XXXXXXXX")
trap 'rm -rf "$test_dir"' EXIT
"${CC:-gcc}" -std=c99 -O1 -g -pthread -fsanitize=address,undefined \
  -fno-omit-frame-pointer -ffunction-sections -fdata-sections \
  -DUSB_HARNESS=1 ${HID_TEST_CFLAGS:-} thidrundown.c ../../src/hidparse.c \
  -Wl,--gc-sections -o "$test_dir/test"
"$test_dir/test"
