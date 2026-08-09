# C++ W Tail-Hedge Backtest

Port of the Python W-strategy engine (`WBacktest.py`, `StrategyW.py`, chain
interpolation, Merton/GBS pricing, expected P/L).

## Build

```bash
cd cpp
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Run

```bash
./build/w_backtest \
  -i ../data/SPY_fixture_small.csv \
  -y ../data/yield-curve-rates-1990-2024.csv \
  --short-dte 30 --long-dte 60 --units 1 --balance 100000 \
  --roll-dte 0 --no-gamma-scalp --bench 20
```

## Parity tests

```bash
# Refresh Python reference (from repo root)
python3 cpp/tools/dump_python_reference.py

# Run C++ checks
./build/w_tests \
  ../data/yield-curve-rates-1990-2024.csv \
  ../data/SPY_fixture_small.csv \
  tools/python_reference.json
```

Checks:

1. Merton/GBS value, delta, IV round-trip vs `optlib`
2. Delta strike picker on first fixture day
3. Full backtest metrics (`annualReturn`, `maxDrawdown`, `finalNLV`, `numRolls`)
4. Prints Python vs C++ wall time and speedup

## Parameter search (`w_search`)

Preferred entry point (builds + loops SPX / QQQ.gz / AAPL):

```bash
./run_cpp_search.sh
# optional overrides:
START=2005-01-01 END=2023-12-31 WORKERS=8 EVERY_N=5 ./run_cpp_search.sh
```

Or call `w_search` directly:

```bash
./build/w_search --symbol QQQ \
  -i ../options-data/opt-prices/QQQ.csv.gz \
  -y ../data/yield-curve-rates-1990-2024.csv \
  --start-date 2005-01-01 --end-date 2023-12-31 --every-n 5 --atm-band 0.20 \
  --units 10 --workers 8 --out search-QQQ-cpp.csv
```

`.csv.gz` inputs are supported (zlib). Grid: short DTE `{5,7,14,30,45}`, long `{30,60,90}`,
deltas `{0.35,0.45}×{0.08,0.12}`, wing modes `fixed(M=2|3)`, `credit`, `theta` (224 configs).


| Dataset | Days | Python | C++ | Speedup |
|---------|------|--------|-----|---------|
| `SPY_fixture_small.csv` | 8 | ~1310 ms | ~11–16 ms | **~80–120×** |
| `SPY_50000.csv` (first ~58 sessions) | 58 | ~9040 ms | ~41 ms | **~220×** |

Parity on the small fixture: GBS matches to ~1e-15; backtest `finalNLV` within ~$1 on a $100k book; `numRolls` identical.

## Notes

- C++ CLI defaults gamma scalp / expected P/L off (same as typical search configs).
- Detail CSV writers are omitted (summary-only style) for speed.
- Weekly/daily DTE synthesis and wing modes (`fixed` / `credit` / `theta`) are implemented in the library; CLI exposes the main knobs.
