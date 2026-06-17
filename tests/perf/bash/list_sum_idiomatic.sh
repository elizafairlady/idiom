#!/usr/bin/env bash
set -euo pipefail
xs=()
for ((i = 0; i < 100000; i++)); do
  xs+=("$i")
done
acc=0
for ((i = ${#xs[@]} - 1; i >= 0; i--)); do
  acc=$(((acc + xs[i]) % 1000000007))
done
printf '%s\n' "$acc"
