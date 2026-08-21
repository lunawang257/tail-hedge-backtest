# W Tail-Hedge Backtest

Backtests a **W** tail-hedge options strategy whose profit/loss curve vs the
underlying looks like the letter **W**. The engine is implemented in C++
(under `cpp/`); the original Python implementation was removed after the port
reached parity (see git history for the Python sources).

## Structure

1. **Short legs** — sell `N_call` near-term calls and `N_put` near-term puts at a
   shared absolute delta (ATM / slightly OTM). Collects premium when markets are quiet.
2. **Long legs** — buy `M_call` / `M_put` same- or longer-dated OTM options with
   a much smaller absolute delta (`M ≥ N`). Wings pay off in tail events.

Assumptions: European options (no early exercise / assignment). Odd target DTEs
are priced by interpolating nearby chains' IV / skew and the treasury curve.

## Adjustment rules (toggle / sweep)

| Rule | Description |
|------|-------------|
| DTE roll | Close & reopen when short-leg DTE ≤ `--roll-dte` (or on expiry) |
| Profit take | Close when structure P/L ≥ profit-take % of \|entry cash\| |
| Loss cut | Close when structure P/L ≤ -loss-cut % (avoid futile gamma scalping) |
| Gamma scalping | When book θ < 0, hedge stock to Δ≈0 once \|Δ\| exceeds threshold |

## Expected P/L

Periodically marks `E[PnL] = Σ PnL(move_i) · P(move_i)` over a move grid, with
lognormal probabilities from realized vol (or empirical returns when enough
history exists).

# Build

```bash
cd cpp
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

This produces three binaries in `cpp/build/`:

- `w_backtest` — single backtest run (CLI)
- `w_search` — multi-threaded parameter grid search
- `w_tests` — parity / regression tests against the frozen Python reference

# Usage

## Single run (`w_backtest`)

```bash
# Bundled SPY fixture under data/
cpp/build/w_backtest \
  -i data/SPY_fixture_small.csv \
  -y data/yield-curve-rates-1990-2024.csv \
  --short-dte 30 --long-dte 60 \
  --short-delta 0.40 --long-delta 0.10 \
  --units 1
```

Key arguments (see `w_backtest --help`):

| Arg | Meaning |
|-----|---------|
| `-i` / `-y` | Options chain CSV / yield curve CSV |
| `--short-dte` / `--long-dte` | Tenors |
| `--short-delta` / `--long-delta` | Absolute deltas |
| `--m-call` / `--m-put` | Long wing contracts per unit |
| `--units` | Scale all contract counts |
| `--roll-dte` | DTE roll trigger (0 = expiry-only) |
| `--start-date` / `--end-date` | Session window (YYYY-MM-DD) |
| `--trade-every-n-days` | Thin sessions for faster sweeps |
| `--gamma-scalp` / `--no-gamma-scalp` | Toggle dynamic hedging (default off) |
| `--no-expected-pnl` | Skip expected P/L marks |
| `--bench N` | Repeat the run N times for timing |

## Parameter search (`w_search`)

Preferred entry point (builds and loops SPX / QQQ / AAPL):

```bash
./run_cpp_search.sh
# optional overrides:
START=2005-01-01 END=2023-12-31 WORKERS=8 EVERY_N=5 ./run_cpp_search.sh
```

See `cpp/README.md` for direct `w_search` invocation, the grid definition,
caching, and parity test instructions.

# Code layout

```
cpp/                  C++ engine, CLI, search, and parity tests (see cpp/README.md)
run_cpp_search.sh     Build + multi-symbol parameter search driver
data/                 Bundled SPY fixtures + yield curve
search-*-cpp.csv      Saved search results
```

# Outputs

- `w_backtest` prints summary metrics (annualized return, max drawdown, final
  NLV, roll & hedge counts) to stdout.
- `w_search` writes one CSV row per parameter set, ranked by score.
