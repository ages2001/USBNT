#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/usbnt-msc-rundown.XXXXXXXX")
trap 'rm -rf "$test_dir"' EXIT
"${CC:-gcc}" -std=c11 -O1 -g -pthread -fsanitize=address,undefined \
  -fno-omit-frame-pointer -ffunction-sections -fdata-sections \
  -DUSB_HARNESS=1 ${MSC_TEST_CFLAGS:-} tmscrundown.c \
  -Wl,--gc-sections -o "$test_dir/test"
"$test_dir/test"
