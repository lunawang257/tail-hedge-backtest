# C++ W Tail-Hedge Backtest

Port of the original Python W-strategy engine (chain interpolation, Merton/GBS
pricing, gamma scalping, expected P/L). The Python implementation has been
removed from the repo; it lives in git history if ever needed.

## Build

```bash
cd cpp
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Run (`w_backtest`)

```bash
./build/w_backtest \
  -i ../data/SPY_fixture_small.csv \
  -y ../data/yield-curve-rates-1990-2024.csv \
  --short-dte 30 --long-dte 60 --units 1 --balance 100000 \
  --roll-dte 0 --profit-take-pct 0.50 --loss-cut-pct 1.00 \
  --no-gamma-scalp --bench 20
```

### Parameter reference

#### Inputs / calendar

| Flag | Meaning |
|------|---------|
| `-i <path>` | Options chain CSV (plain or `.csv.gz`). Required in practice; defaults to a small SPY fixture if omitted. |
| `-y <path>` | Yield-curve CSV used for risk-free rates (and dividend yield lookups) in Merton/GBS pricing. |
| `--start-date YYYY-MM-DD` | Inclusive first session to load. Omit to use the first date in the file. |
| `--end-date YYYY-MM-DD` | Inclusive last session to load. Omit to use the last date in the file. |
| `--trade-every-n-days N` | Thin the calendar: keep every Nth trading session (default `1` = every day). |

#### W-structure geometry

The strategy sells a short-dated call+put body and buys longer-dated wings
(a “W” / iron-condor-like calendar structure).

| Flag | Meaning |
|------|---------|
| `--short-dte N` | Target days-to-expiry for the **short** (sold) body legs. Default `30`. |
| `--long-dte N` | Target days-to-expiry for the **long** (bought) wing legs. Default `60`. |
| `--short-delta X` | Absolute delta target for short call/put selection (e.g. `0.40` ≈ 40Δ). Default `0.40`. |
| `--long-delta X` | Absolute delta target for long wing selection (further OTM). Default `0.10`. |
| `--units N` | Multiplier applied to all four legs (scales size). Default `1`. |
| `--m-call N` | In fixed wing mode: number of long-call wing contracts per unit body. Default `2`. |
| `--m-put N` | In fixed wing mode: number of long-put wing contracts per unit body. Default `2`. |
| `--balance X` | Starting cash / NLV. Default `1e6` (`100000` in the example above). |
| `--commission X` | Option commission in $/contract. Default `1.7125`. |
| `--slippage-pct X` | Option slippage as a **percent of mid** (e.g. `5` = 5%). Default `5`. |

CLI `w_backtest` uses **fixed** wing mode (`m_call` / `m_put`). Credit / theta
wing sizing is available in the library and via `w_search`.

#### Exit / roll rules

At least one exit path should stay enabled or positions hold until the last bar
(`final` close).

| Flag | Meaning |
|------|---------|
| `--roll-dte N` | When DTE-roll is on, close+reopen once the nearest short leg’s DTE is `≤ N`. Default `0` (roll at/after expiry DTE). |
| `--dte-roll` / `--no-dte-roll` | Enable / disable DTE-based rolls. Default: enabled. |
| `--roll-on-expiry` / `--no-roll-on-expiry` | When DTE-roll is on, also treat `short DTE ≤ 0` as an expiry roll reason. Default: enabled. |
| `--profit-take-pct X` | Close when structure P/L ≥ `X × |entry cash|` (e.g. `0.50` = take 50% of credit/debit). Enables profit-take. Default `0.50`. |
| `--profit-take` / `--no-profit-take` | Enable / disable profit-take exits. Default: enabled. |
| `--loss-cut-pct X` | Close when structure P/L ≤ `-X × |entry cash|`. Enables loss-cut. Default `1.00`. |
| `--loss-cut` / `--no-loss-cut` | Enable / disable loss-cut exits. Default: enabled. |

Close **reasons** written in verbose mode: `dte_roll`, `expire`, `profit_take`,
`loss_cut`, `final` (forced flatten on the last bar), plus `open` for entries.

#### Runtime / diagnostics

| Flag | Meaning |
|------|---------|
| `--gamma-scalp` / `--no-gamma-scalp` | Hedge residual delta with stock when gamma scalping is on. Default: **off**. |
| `--no-expected-pnl` | Skip periodic expected-P/L evaluation (default already off in CLI). |
| `--verbose` / `-v` | Record every option open/close into a CSV (see below). |
| `--verbose-out <path>` | Path for the verbose trade CSV (implies `--verbose`). Default `transactions.csv`. |
| `--bench N` | Run the backtest `N` times and report average wall time. Trade CSV is written from the last run only. |
| `-h` / `--help` | Print usage. |

### Verbose trade CSV

```bash
./build/w_backtest \
  -i ../data/SPY_fixture_small.csv \
  -y ../data/yield-curve-rates-1990-2024.csv \
  --short-dte 30 --long-dte 60 --units 1 --balance 100000 \
  --verbose --verbose-out trades.csv
```

Columns:

| Column | Meaning |
|--------|---------|
| `date` | Session date of the fill |
| `action` | `open` or `close` |
| `structureId` | Monotonic id of the W structure |
| `leg` | `short_call` / `short_put` / `long_call` / `long_put` |
| `type` | `c` or `p` |
| `strike` | Option strike |
| `expiry` | Option expiration date |
| `contracts` | Signed size (`+` long / `-` short) |
| `openPrice` | Per-share entry fill (incl. commission allocation) |
| `closePrice` | Per-share exit fill on closes (`0` on opens) |
| `pnl` | Realized capital gain on closes (`0` on opens) |
| `reason` | Why the fill happened (`open`, `dte_roll`, `expire`, `profit_take`, `loss_cut`, `final`) |
| `spot` | Underlying price on the fill date |

## Parity tests

```bash
./build/w_tests \
  ../data/yield-curve-rates-1990-2024.csv \
  ../data/SPY_fixture_small.csv \
  tools/python_reference.json
```

`tools/python_reference.json` is a frozen baseline generated from the original
Python implementation (via `cpp/tools/dump_python_reference.py`, now removed —
recover both from git history if the fixture ever needs regenerating).

Checks:

1. Merton/GBS value, delta, IV round-trip vs `optlib` reference values
2. Delta strike picker on first fixture day
3. Full backtest metrics (`annualReturn`, `maxDrawdown`, `finalNLV`, `numRolls`)
4. Prints recorded Python vs C++ wall time and speedup

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

### `w_search` parameters

| Flag | Meaning |
|------|---------|
| `--symbol SYM` | Label written into logs / default output name (e.g. `QQQ`). |
| `-i <path>` | Options chain CSV / `.csv.gz` to search over. **Required.** |
| `-y <path>` | Yield-curve CSV. |
| `--cache <path>` | Binary dataset cache path (built automatically if missing). |
| `--start-date` / `--end-date` | Inclusive date window for the search sample. |
| `--every-n N` | Session thinning (default `5`) — keeps every Nth trading day to speed the grid. |
| `--atm-band X` | Keep strikes within ±`X` fraction of spot when loading (default `0.20` = ±20%). |
| `--units N` | Structure size for every grid config (default `10`). |
| `--balance X` | Starting cash for each backtest (default `1e6`). |
| `--commission X` | Option commission $/contract (default `1.7125`). |
| `--slippage-pct X` | Option slippage % of mid; `5` = 5% (default `5`). |
| `--workers N` | Thread count for **one** symbol’s grid (default: hardware concurrency). |
| `--out <path>` | Results CSV path (default `search-<SYM>-cpp.csv`). |
| `--limit N` | Debug: evaluate only the first `N` grid configs. |

Within one symbol, configs are evaluated in parallel across `--workers` threads (same dataset, different params). `run_cpp_search.sh` defaults `WORKERS` to `hw.ncpu`.

### Search grid

Defined in `cpp/src/w_search.cpp` (`GridSpec` / `build_grid`). Defaults:

| Dimension | Values | Meaning |
|-----------|--------|---------|
| short DTE | `{5,7,14,30,45}` | Body tenor |
| long DTE | `{30,60,90}` (must be ≥ short) | Wing tenor |
| short Δ | `{0.35,0.45}` | Body delta target |
| long Δ | `{0.08,0.12}` (must be &lt; short Δ) | Wing delta target |
| wing mode | `fixed(M=2\|3)`, `credit`, `theta` | How wing contract counts are chosen |
| `rollDTE` | `{0,3,7}` | DTE roll threshold |
| `profitTake` | `{off,0.25,0.50,0.75}` | Profit-take fraction or disabled |
| `lossCut` | `{off,0.50,1.00}` | Loss-cut fraction or disabled |

That’s **8064** configs (DTE roll + expiry always on in the grid). Score =
`annualReturn - 0.5 × maxDrawdown`.

Hard rule: long wing contracts must always exceed short body contracts
(`m > n`, with search body `n=1` so `M≥2`). `fixed:1` is rejected; credit /
theta size from at least `n+1` upward.

Shrink or pin any dimension without rebuilding (comma lists; one value = fixed):

```bash
./build/w_search ... --short-dtes 30 --long-dtes 60,90 \
  --wings fixed:2 --roll-dtes 0 --profit-takes 0.50 --loss-cuts off
```

| Flag | Example |
|------|---------|
| `--short-dtes` | `30` or `5,14,30` |
| `--long-dtes` | `60,90` |
| `--short-deltas` / `--long-deltas` | `0.45` / `0.08,0.12` |
| `--wings` | `fixed:2,fixed:3,credit,theta` |
| `--roll-dtes` | `0,7` |
| `--profit-takes` / `--loss-cuts` | `off,0.50` (use `off` to disable) |

Wing modes:

- **fixed(M)**: buy `M` wing contracts per body unit (`m_call = m_put = M`,
  `M ≥ 2`).
- **credit**: increase wing size from `n+1` up to `maxWingMult` while keeping
  long debit ≤ short credit × `(1 - creditBuffer)` (never below `n+1`).
- **theta**: increase wing size from `n+1` while net structure theta ≥
  `thetaThreshold` (never below `n+1`).

| Dataset | Days | Python | C++ | Speedup |
|---------|------|--------|-----|---------|
| `SPY_fixture_small.csv` | 8 | ~1310 ms | ~11–16 ms | **~80–120×** |
| `SPY_50000.csv` (first ~58 sessions) | 58 | ~9040 ms | ~41 ms | **~220×** |

Parity on the small fixture: GBS matches to ~1e-15; backtest `finalNLV` within ~$1 on a $100k book; `numRolls` identical.

## Notes

- C++ CLI defaults gamma scalp / expected P/L off (same as typical search configs).
- Use `--verbose` on `w_backtest` for per-leg trade CSVs; search stays summary-only for speed.
- Weekly/daily DTE synthesis and wing modes (`fixed` / `credit` / `theta`) are implemented in the library; `w_search` sweeps them, `w_backtest` exposes fixed wings via `--m-call` / `--m-put`.
