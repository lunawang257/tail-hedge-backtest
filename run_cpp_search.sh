#!/usr/bin/env bash
# Build C++ W backtest and run parameter search.
#
# Usage:
#   ./run_cpp_search.sh                 # all symbols, no tax (default)
#   ./run_cpp_search.sh -tax            # all symbols, default CA+federal tax
#   ./run_cpp_search.sh -no-tax NVDA    # listed symbols, 0 tax
#   ./run_cpp_search.sh NVDA GOOG -tax
#
# Symbols: SPX QQQ AAPL (START→END window), NVDA GOOG AVGO (full history).
# Flags: -tax / --tax (37.1% LT / 54.1% ST), -no-tax / --no-tax (0%; default).
# Env overrides: OPTIONS_DATA YIELD START END EVERY_N ATM_BAND UNITS
#   LONG_POS_MIL HEDGE_RATIOS CALL_PUT_RATIO_PCTS LT_TAX_PCT ST_TAX_PCT
#   WORKERS BUILD_DIR LOG TOP10_OUT
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
LONG_POS_MIL="${LONG_POS_MIL:-10}"
HEDGE_RATIOS="${HEDGE_RATIOS:-0.5,1,1.5,2}"
CALL_PUT_RATIO_PCTS="${CALL_PUT_RATIO_PCTS:-100,50,25}"
LT_TAX_PCT="${LT_TAX_PCT:-37.1}"
ST_TAX_PCT="${ST_TAX_PCT:-54.1}"
WORKERS="${WORKERS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 8)}"
BUILD_DIR="${BUILD_DIR:-$ROOT/cpp/build}"
LOG="${LOG:-$ROOT/cpp-search-${START}-${END}.log}"
TOP10_OUT="${TOP10_OUT:-$ROOT/search-top10-cpp.csv}"
# Wide bounds so w_search loads the entire file (CLI requires dates).
FULL_START="${FULL_START:-1990-01-01}"
FULL_END="${FULL_END:-2099-12-31}"

usage() {
  cat <<EOF
Usage: $0 [-tax|-no-tax] [SYMBOL ...]

  -tax, --tax        Apply default capital-gain tax (lt=${LT_TAX_PCT}% st=${ST_TAX_PCT}%)
  -no-tax, --no-tax  No tax (0%; default)

Symbols: SPX QQQ AAPL NVDA GOOG AVGO (aliases: NVIDIA, GOOGLE, GOOGL).
Omit symbols to run the full catalog.
EOF
}

# Default: no tax. -tax uses LT_TAX_PCT / ST_TAX_PCT (37.1 / 54.1 unless overridden).
USE_TAX=0
declare -a SYM_ARGS=()
for arg in "$@"; do
  case "$arg" in
    -tax|--tax) USE_TAX=1 ;;
    -no-tax|--no-tax) USE_TAX=0 ;;
    -h|--help)
      usage
      exit 0
      ;;
    -*)
      echo "error: unknown flag: $arg" >&2
      usage >&2
      exit 1
      ;;
    *)
      SYM_ARGS+=("$arg")
      ;;
  esac
done
if [[ "$USE_TAX" -eq 0 ]]; then
  RUN_LT_TAX_PCT=0
  RUN_ST_TAX_PCT=0
else
  RUN_LT_TAX_PCT="$LT_TAX_PCT"
  RUN_ST_TAX_PCT="$ST_TAX_PCT"
fi

normalize_symbol() {
  local s
  s=$(printf '%s' "$1" | tr '[:lower:]' '[:upper:]')
  case "$s" in
    NVIDIA) echo NVDA ;;
    GOOGLE|GOOGL) echo GOOG ;;
    *) echo "$s" ;;
  esac
}

# Resolve options CSV path: prefer uncompressed .csv, else .csv.gz.
resolve_input() {
  local sym="$1"
  if [[ "$sym" == "SPX" ]]; then
    echo "$OPTIONS_DATA/SPX.csv"
    return
  fi
  local base="$OPTIONS_DATA/opt-prices/$sym"
  if [[ -f "$base.csv" ]]; then
    echo "$base.csv"
  elif [[ -f "$base.csv.gz" ]]; then
    echo "$base.csv.gz"
  else
    echo "$base.csv.gz"
  fi
}

# Catalog: symbol|window (window uses START/END; full uses entire history)
declare -a ALL_JOBS=(
  "SPX|window"
  "QQQ|window"
  "AAPL|window"
  "NVDA|full"
  "GOOG|full"
  "AVGO|full"
)

job_for_symbol() {
  local want="$1" job sym
  for job in "${ALL_JOBS[@]}"; do
    IFS='|' read -r sym _ <<< "$job"
    if [[ "$sym" == "$want" ]]; then
      echo "$job"
      return 0
    fi
  done
  return 1
}

declare -a SELECTED=()
if [[ ${#SYM_ARGS[@]} -eq 0 ]]; then
  for job in "${ALL_JOBS[@]}"; do
    SELECTED+=("$job")
  done
else
  for arg in "${SYM_ARGS[@]}"; do
    SYM="$(normalize_symbol "$arg")"
    if job=$(job_for_symbol "$SYM"); then
      SELECTED+=("$job")
    else
      echo "error: unknown symbol: $arg (normalized $SYM)" >&2
      echo "known: SPX QQQ AAPL NVDA GOOG AVGO (aliases: NVIDIA, GOOGLE, GOOGL)" >&2
      exit 1
    fi
  done
  # Re-order to catalog order and drop duplicates.
  declare -a ORDERED=()
  for job in "${ALL_JOBS[@]}"; do
    for sel in "${SELECTED[@]}"; do
      if [[ "$sel" == "$job" ]]; then
        ORDERED+=("$job")
        break
      fi
    done
  done
  SELECTED=("${ORDERED[@]}")
fi

# Fail fast for explicitly requested symbols; when running all, skip missing inputs.
declare -a RUNNABLE=()
for job in "${SELECTED[@]}"; do
  IFS='|' read -r SYM _ <<< "$job"
  INPUT="$(resolve_input "$SYM")"
  if [[ -f "$INPUT" ]]; then
    RUNNABLE+=("$job")
  elif [[ ${#SYM_ARGS[@]} -gt 0 ]]; then
    echo "error: missing input for $SYM: $INPUT" >&2
    exit 1
  else
    echo "warning: skipping $SYM (missing $INPUT)" >&2
  fi
done
if [[ ${#RUNNABLE[@]} -eq 0 ]]; then
  echo "error: no runnable symbols (all inputs missing)" >&2
  exit 1
fi
SELECTED=("${RUNNABLE[@]}")

echo "=== Building C++ (Release) ==="
cmake -S "$ROOT/cpp" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" -j "$WORKERS" --target w_search w_backtest w_tests

W_SEARCH="$BUILD_DIR/w_search"
if [[ ! -x "$W_SEARCH" ]]; then
  echo "error: missing $W_SEARCH" >&2
  exit 1
fi

mkdir -p "$ROOT/data/cache"
: > "$LOG"
# Combined top-10 CSV (symbol as first column); header written on first append.
: > "$TOP10_OUT"
TOP10_HEADER_WRITTEN=0

echo "Logging to $LOG"
echo "Window $START → $END (windowed symbols); full-history symbols use entire file"
echo "every_n=$EVERY_N  workers=$WORKERS  units=$UNITS  long_pos_mil=$LONG_POS_MIL"
if [[ "$LONG_POS_MIL" != "0" ]]; then
  echo "hedge_ratios=$HEDGE_RATIOS"
fi
if [[ "$USE_TAX" -eq 1 ]]; then
  echo "tax=-tax  lt_tax_pct=$RUN_LT_TAX_PCT  st_tax_pct=$RUN_ST_TAX_PCT"
else
  echo "tax=-no-tax  lt_tax_pct=$RUN_LT_TAX_PCT  st_tax_pct=$RUN_ST_TAX_PCT"
fi
echo "call_put_ratio_pcts=$CALL_PUT_RATIO_PCTS"
echo "Symbols: $(printf '%s ' "${SELECTED[@]%%|*}")"
echo "Top-10 combined → $TOP10_OUT"

append_top10() {
  local sym="$1"
  local out="$2"
  local header
  header=$(head -n 1 "$out")
  if [[ "$TOP10_HEADER_WRITTEN" -eq 0 ]]; then
    echo "symbol,$header" > "$TOP10_OUT"
    TOP10_HEADER_WRITTEN=1
  fi
  # Results CSV is already sorted by score descending.
  # Avoid SIGPIPE under pipefail from `tail | head`.
  local n=0
  while IFS= read -r line && [[ $n -lt 10 ]]; do
    echo "$sym,$line" >> "$TOP10_OUT"
    n=$((n + 1))
  done < <(tail -n +2 "$out")
}

declare -a RUNTIMES=()
TOTAL_T0=$(date +%s)

for job in "${SELECTED[@]}"; do
  IFS='|' read -r SYM WINDOW <<< "$job"
  INPUT="$(resolve_input "$SYM")"
  if [[ ! -f "$INPUT" ]]; then
    echo "error: missing input for $SYM: $INPUT" >&2
    exit 1
  fi

  if [[ "$WINDOW" == "full" ]]; then
    RUN_START="$FULL_START"
    RUN_END="$FULL_END"
    CACHE="$ROOT/data/cache/${SYM}_full_e${EVERY_N}_atm.bin"
    OUT="$ROOT/search-${SYM}-cpp-full.csv"
  else
    RUN_START="$START"
    RUN_END="$END"
    CACHE="$ROOT/data/cache/${SYM}_${START//-}_${END//-}_e${EVERY_N}_atm.bin"
    OUT="$ROOT/search-${SYM}-cpp-${START}-${END}.csv"
  fi

  echo ""
  echo "===== $SYM ($WINDOW) =====" | tee -a "$LOG"
  echo "input=$INPUT" | tee -a "$LOG"
  echo "dates=$RUN_START → $RUN_END" | tee -a "$LOG"
  echo "iv-smile=$(dirname "$INPUT")/${SYM}-ivsmile.csv (default beside data)" | tee -a "$LOG"
  SYM_T0=$(date +%s)
  SEARCH_ARGS=(
    --symbol "$SYM"
    -i "$INPUT" -y "$YIELD"
    --cache "$CACHE"
    --start-date "$RUN_START" --end-date "$RUN_END"
    --every-n "$EVERY_N" --atm-band "$ATM_BAND"
    --units "$UNITS" --workers "$WORKERS"
    --lt-tax-pct "$RUN_LT_TAX_PCT" --st-tax-pct "$RUN_ST_TAX_PCT"
    --call-put-ratio-pcts "$CALL_PUT_RATIO_PCTS"
    --out "$OUT"
  )
  if [[ "$LONG_POS_MIL" != "0" ]]; then
    SEARCH_ARGS+=(--long-pos-mil "$LONG_POS_MIL" --hedge-ratios "$HEDGE_RATIOS")
  fi
  "$W_SEARCH" "${SEARCH_ARGS[@]}" 2>&1 | tee -a "$LOG"
  SYM_T1=$(date +%s)
  SYM_SEC=$((SYM_T1 - SYM_T0))
  RUNTIMES+=("$SYM|$SYM_SEC|$OUT")
  append_top10 "$SYM" "$OUT"
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
echo "  top10 combined → $TOP10_OUT" | tee -a "$LOG"
echo "  total runtime: ${TOTAL_SEC}s ($(awk -v s="$TOTAL_SEC" 'BEGIN{printf "%.1f min", s/60}'))" | tee -a "$LOG"
