#!/usr/bin/env bash
# Build C++ W backtest and run parameter search for SPX, QQQ, AAPL.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

OPTIONS_DATA="${OPTIONS_DATA:-$ROOT/../options-data}"
YIELD="${YIELD:-$ROOT/data/yield-curve-rates-1990-2024.csv}"
START="${START:-2005-01-01}"
END="${END:-2023-12-31}"
EVERY_N="${EVERY_N:-5}"
ATM_BAND="${ATM_BAND:-0.20}"
UNITS="${UNITS:-10}"
WORKERS="${WORKERS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 8)}"
BUILD_DIR="${BUILD_DIR:-$ROOT/cpp/build}"
LOG="${LOG:-$ROOT/cpp-search-${START}-${END}.log}"

echo "=== Building C++ (Release) ==="
cmake -S "$ROOT/cpp" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" -j "$WORKERS" --target w_search w_backtest w_tests

W_SEARCH="$BUILD_DIR/w_search"
if [[ ! -x "$W_SEARCH" ]]; then
  echo "error: missing $W_SEARCH" >&2
  exit 1
fi

# symbol|input_csv|cache_bin|out_csv
declare -a JOBS=(
  "SPX|$OPTIONS_DATA/SPX.csv|$ROOT/data/cache/SPX_${START//-}_${END//-}_e${EVERY_N}_atm.bin|$ROOT/search-SPX-cpp-${START}-${END}.csv"
  "QQQ|$OPTIONS_DATA/opt-prices/QQQ.csv.gz|$ROOT/data/cache/QQQ_${START//-}_${END//-}_e${EVERY_N}_atm.bin|$ROOT/search-QQQ-cpp-${START}-${END}.csv"
  "AAPL|$OPTIONS_DATA/opt-prices/AAPL.csv|$ROOT/data/cache/AAPL_${START//-}_${END//-}_e${EVERY_N}_atm.bin|$ROOT/search-AAPL-cpp-${START}-${END}.csv"
)

mkdir -p "$ROOT/data/cache"
: > "$LOG"
echo "Logging to $LOG"
echo "Window $START → $END  every_n=$EVERY_N  workers=$WORKERS  units=$UNITS"

declare -a RUNTIMES=()
TOTAL_T0=$(date +%s)

for job in "${JOBS[@]}"; do
  IFS='|' read -r SYM INPUT CACHE OUT <<< "$job"
  if [[ ! -f "$INPUT" ]]; then
    echo "error: missing input for $SYM: $INPUT" >&2
    exit 1
  fi
  echo ""
  echo "===== $SYM =====" | tee -a "$LOG"
  echo "input=$INPUT" | tee -a "$LOG"
  SYM_T0=$(date +%s)
  "$W_SEARCH" --symbol "$SYM" \
    -i "$INPUT" -y "$YIELD" \
    --cache "$CACHE" \
    --start-date "$START" --end-date "$END" \
    --every-n "$EVERY_N" --atm-band "$ATM_BAND" \
    --units "$UNITS" --workers "$WORKERS" \
    --out "$OUT" 2>&1 | tee -a "$LOG"
  SYM_T1=$(date +%s)
  SYM_SEC=$((SYM_T1 - SYM_T0))
  RUNTIMES+=("$SYM|$SYM_SEC|$OUT")
  echo "Wrote $OUT" | tee -a "$LOG"
  echo "Runtime $SYM: ${SYM_SEC}s ($(awk -v s="$SYM_SEC" 'BEGIN{printf "%.1f min", s/60}'))" | tee -a "$LOG"
done

TOTAL_T1=$(date +%s)
TOTAL_SEC=$((TOTAL_T1 - TOTAL_T0))

echo ""
echo "=== ALL DONE ===" | tee -a "$LOG"
for entry in "${RUNTIMES[@]}"; do
  IFS='|' read -r SYM SEC OUT <<< "$entry"
  echo "  $SYM → $OUT  (${SEC}s, $(awk -v s="$SEC" 'BEGIN{printf "%.1f min", s/60}'))" | tee -a "$LOG"
done
echo "  total runtime: ${TOTAL_SEC}s ($(awk -v s="$TOTAL_SEC" 'BEGIN{printf "%.1f min", s/60}'))" | tee -a "$LOG"
