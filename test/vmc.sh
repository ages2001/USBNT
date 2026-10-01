#!/bin/sh
# vmc.sh <dir> <python code>: runs code in the vmd.py VM and prints the result
rm -f "$1/done"
printf '%s' "$2" > "$1/cmd" &
n=0
while [ ! -f "$1/done" ] && [ ! "$(cat $1/out 2>/dev/null)" = "closed" ]; do
  sleep 0.3; n=$((n+1))
  if [ $n -gt 1500 ]; then echo "vmc: no answer"; exit 1; fi
  if [ $((n % 10)) -eq 0 ] && ! kill -0 "$(cat $1/pid 2>/dev/null)" 2>/dev/null; then echo "vmc: vmd is gone"; exit 1; fi
done
cat "$1/out"
