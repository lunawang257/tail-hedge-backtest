#!/usr/bin/env bash
# Quick SPY smoke run using bundled data/ sample (no external projects needed).
set -euo pipefail
cd "$(dirname "$0")"
python3 WBacktest.py \
  -i data/SPY_50000.csv \
  -y data/yield-curve-rates-1990-2024.csv \
  -o w-spy-details.csv \
  -s w-spy-summary.csv \
  --short-dte 30 \
  --long-dte 60 \
  --short-delta 0.40 \
  --long-delta 0.10 \
  --units 1 \
  "$@"
