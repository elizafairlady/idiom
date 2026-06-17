#!/usr/bin/env bash
set -euo pipefail
acc=0
for ((i = 0; i < 500000; i++)); do
  acc=$(((acc + (i * 3) + 7) % 1000000007))
done
printf '%s\n' "$acc"
