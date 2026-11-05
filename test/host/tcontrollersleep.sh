#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
cc -std=c99 -Wall -Wextra -Werror -Wno-misleading-indentation \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Isrc test/host/tcontrollersleep.c -o "$tmp/test"
"$tmp/test"
