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
  --short-dte 30 --long-dte 60 --units 1 \
  --roll-dte 0 --profit-take-pct 0.50 --loss-cut-pct 1.00 \
  --no-gamma-scalp --bench 20
```

### Parameter reference

#### Inputs / calendar

| Flag | Meaning |
|------|---------|
| `-i <path>` | Options chain CSV (plain or `.csv.gz`). Required in practice; defaults to a small SPY fixture if omitted. |
| `-y <path>` | Yield-curve CSV used for risk-free rates (and dividend yield lookups) in Merton/GBS pricing. |
| `--iv-smile <path>` | IV smile CSV (default: `<data-dir>/<stem>-ivsmile.csv`). |
| `--merge-split <path>` | Merge/split CSV (default: `<data-dir>/<stem>-merge-split.csv`, beside `-i`). Scales option and stock commissions by remaining later split factors (2:1 then 7:1 → `1/14` of today's rate before both, `1/7` between). Missing default file → unadjusted commissions. |
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
| `--units N` | Multiplier applied to all four legs (scales size). Default `1`. Ignored when `--long-pos-mil > 0`. |
| `--m-call X` | In fixed wing mode: long/short **ratio** for calls (e.g. `1.3` → 13 longs per 10 shorts). Default `2`. |
| `--m-put X` | In fixed wing mode: long/short **ratio** for puts. Default `2`. |
| `--call-put-ratio-pct X` | Call contracts as a **percent of puts** (default `100` = equal). `--call-put-ratio-pct 20` with puts −100/+200 → calls −20/+40. |
| `--long-pos-mil X` | Starting capital in $millions (default `0` = options-only, `$1e6` cash). Buys `X*1e6` notional on day 1; NLV/MaxDD include W + stock. Alias: `--long-pos`. Opening W credit is held as cash (not swept into overlay). Excess cash above that credit buys overlay shares; a shortfall sells highest cost-basis lots. Year-end tax is paid from cash / by selling stock; unused capital losses carry forward. |
| `--hedge-ratio X` | With long-pos: size W so net long puts ≈ `X × (shares/100)`. |
| `--commission X` | Option commission in $/contract **today**. Default `1.7125`. Dates before later splits are scaled via `--merge-split`. |
| `--slippage-pct X` | Option slippage as a **percent of mid** (e.g. `5` = 5%). Default `5`. |
| `--lt-tax-pct X` | Long-term capital-gain tax percent (holding **> 1 year**). Default `37.1`. |
| `--st-tax-pct X` | Short-term capital-gain tax percent (holding **≤ 1 year**). Default `54.1`. |

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
`loss_cut`, `final` (forced flatten on the last bar), plus `open` for option
entries. Overlay stock: `overlay_open` (day-1 buy), `overlay_buy` / `overlay_sell`
(rebalance cash to the open W credit), `tax` (sell to pay year-end tax, and the tax
row itself).

#### Runtime / diagnostics

| Flag | Meaning |
|------|---------|
| `--gamma-scalp` / `--no-gamma-scalp` | Hedge residual delta with stock when gamma scalping is on. Default: **off**. |
| `--no-expected-pnl` | Skip periodic expected-P/L evaluation (default already off in CLI). |
| `--verbose` / `-v` | Record every option and stock fill into a CSV (see below). |
| `--verbose-out <path>` | Path for the verbose trade CSV (implies `--verbose`). Default `transactions.csv`. |
| `--verbose-log <path>` | Write a collapsible HTML trade log (implies recording fills). Fold/unfold by year and day; includes **Fold all** / **Unfold all**. |
| `--bench N` | Run the backtest `N` times and report average wall time. Trade CSV/HTML are written from the last run only. |
| `-h` / `--help` | Print usage. |

### Verbose trade CSV

```bash
./build/w_backtest \
  -i ../data/SPY_fixture_small.csv \
  -y ../data/yield-curve-rates-1990-2024.csv \
  --short-dte 30 --long-dte 60 --units 1 \
  --verbose --verbose-out trades.csv
```

Columns:

| Column | Meaning |
|--------|---------|
| `date` | Session date of the fill |
| `action` | `open` / `close` (options), `buy` / `sell` (stock), `tax` (year-end tax paid) |
| `structureId` | Monotonic id of the W structure (`0` before the first open) |
| `leg` | `short_call` / `short_put` / `long_call` / `long_put` / `stock` / `tax` |
| `type` | `c` or `p` (options), `s` (stock), `t` (tax) |
| `strike` | Option strike (`0` for stock/tax) |
| `expiry` | Option expiration date (empty for stock/tax) |
| `contracts` | Signed option size (`+` long / `-` short); `0` for stock/tax |
| `shares` | Signed stock size (`+` buy / `-` sell); `0` for options/tax |
| `openPrice` | Per-share entry fill (incl. commission allocation) |
| `closePrice` | Per-share exit fill on closes/sells (`0` on opens/buys) |
| `pnl` | Realized capital gain on closes/sells (`0` on opens/buys); tax amount as negative on `tax` rows |
| `reason` | Why the fill happened (`open`, `dte_roll`, `expire`, `profit_take`, `loss_cut`, `final`, `overlay_open`, `overlay_buy`, `overlay_sell`, `tax`) |
| `spot` | Underlying price on the fill date |
| `hedgeRatio` | Target hedge ratio (`--hedge-ratio`); `0` when long-pos overlay is off |
| `callPutRatioPct` | Call contracts as % of puts (`--call-put-ratio-pct`) |
| `bhShares` | Buy-and-hold overlay shares after this fill |
| `totalTax` | Cumulative tax paid so far |
| `stLossCarry` / `ltLossCarry` | Unused ST/LT capital-loss carryover (`>0` means unused loss) |
| `stGain` / `ltGain` | Year-to-date realized ST/LT capital gain (negative means loss) |
| `cash` | Cash balance after this fill |
| `stockNlv` | Stock mark (`shares × spot`) after this fill |
| `totalNlv` | `cash + stockNlv +` option mark-to-market (W-strategy + stock) |
| `costBasis` | Dollar cost of shares sold (`0` on buys/options) |
| `tradeStGain` / `tradeLtGain` | This fill's ST/LT capital gain (negative means loss) |

### Verbose HTML log

```bash
./build/w_backtest \
  -i ../data/SPY_fixture_small.csv \
  -y ../data/yield-curve-rates-1990-2024.csv \
  --short-dte 30 --long-dte 60 --units 1 \
  --verbose-log trades.html
```

Each day with a buy/sell **or a new max-drawdown trough** is nested under year → day. Days start folded (`[+]` to expand). The page toolbar has **Fold all** and **Unfold all**. A foldable **Reproduce command** at the top copies the full CLI that regenerates this file.

Each day shows:

- Header: date, spot, NLV, commission, peak, max drawdown (and date), ST/LT loss carry, ST/LT YTD gain, buy-and-hold NLV / maxDD / maxDD date
- Actions (trade days): **Open W Strategy**, **Close W Strategy** (reason + P/L), stock buy/sell (shares, price, reason), year-end tax
- Position table: option legs (mark-to-market), **Options NLV**, **Stocks NLV**, cash, NLV

`--verbose-log` can be combined with `--verbose` / `--verbose-out` to also write the CSV.

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
./run_cpp_search.sh                 # all symbols, no tax (default)
./run_cpp_search.sh -tax            # default CA+federal tax (37.1% LT / 54.1% ST)
./run_cpp_search.sh -no-tax NVDA    # listed symbols, 0 tax
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
| `--iv-smile <path>` | IV smile CSV (default: `<data-dir>/<SYM>-ivsmile.csv`). |
| `--merge-split <path>` | Merge/split CSV (default: `<data-dir>/<SYM>-merge-split.csv`, same directory as `-i`). Scales option and stock commissions by the product of **later** split factors: after a 2:1 then 7:1, rates before both are `1/14` of today's, and `1/7` between the two splits. Missing default file → unadjusted commissions. |
| `--start-date` / `--end-date` | Inclusive date window for the search sample. |
| `--every-n N` | Session thinning (default `5`) — keeps every Nth trading day to speed the grid. |
| `--atm-band X` | Keep strikes within ±`X` fraction of spot when loading (default `0.20` = ±20%). |
| `--units N` | Structure size for every grid config (default `10`). Ignored when `--long-pos-mil > 0` (size comes from hedge ratio). |
| `--long-pos-mil X` | Starting capital in **$millions** (default `0` = options-only). Starts with `X*1e6` cash, buys that notional of the symbol on day 1. Opening W credit is held as cash. Combined NLV/MaxDD = W-strategy + long stock. Prints a buy-and-hold-only baseline. Alias: `--long-pos`. |
| `--hedge-ratio X` | Single hedge ratio (only with long-pos). Net long puts (`long − short` put contracts) protect `X × (shares/100)` of the long. |
| `--hedge-ratios a,b,..` | Grid of hedge ratios when long-pos is on (default `0.5,1,1.5,2`). |
| `--call-put-ratio-pct X` | Single value: call contracts as % of puts (`100` = equal). |
| `--call-put-ratio-pcts a,b,..` | Grid of call/put percents (default `100,50,25`). `20` → calls are 20% of puts. |
| `--commission X` | Option commission in $/contract **today** (default `1.7125`). Dates before later splits are scaled via `--merge-split`. |
| `--slippage-pct X` | Option slippage % of mid; `5` = 5% (default `5`). |
| `--lt-tax-pct X` | Long-term capital-gain tax % (default `37.1`). |
| `--st-tax-pct X` | Short-term capital-gain tax % (default `54.1`). |
| `--workers N` | Thread count for **one** symbol’s grid (default: hardware concurrency). |
| `--out <path>` | Results CSV path (default `search-<SYM>-cpp.csv`). |
| `--verbose-log <path>` | After the grid, re-run the **best-scoring** config and write a collapsible HTML trade log (same format as `w_backtest --verbose-log`). |
| `--limit N` | Debug: evaluate only the first `N` grid configs. |

Within one symbol, configs are evaluated in parallel across `--workers` threads (same dataset, different params). `run_cpp_search.sh` defaults `WORKERS` to `hw.ncpu`.

### Search grid

Defined in `cpp/src/w_search.cpp` (`GridSpec` / `build_grid`). Defaults:

| Dimension | Values | Meaning |
|-----------|--------|---------|
| short DTE | `{7,14,30,45}` | Body tenor |
| long DTE | `{30,60,90}` (must be ≥ short) | Wing tenor |
| short Δ | `{0.35,0.45}` | Body delta target |
| long Δ | `{0.08,0.12}` (must be &lt; short Δ) | Wing delta target |
| wing mode | `fixed(ratio=2)`, `credit`, `theta` | How wing contract counts are chosen |
| `rollDTE` | `{0}` | DTE roll threshold |
| `profitTake` | `{off,0.75}` | Profit-take fraction or disabled |
| `lossCut` | `{off}` | Loss-cut disabled in default grid |
| hedge ratio | `{0.5,1,1.5,2}` when `--long-pos-mil > 0` | Net-put protection vs long stock |
| call/put ratio | `{100,50,25}` | Call contracts as % of puts (`100` = equal) |

Score = `annualReturn - 0.5 × maxDrawdown`. Top-10 / CSV also report
`taxPct` = cumulative tax paid / final NLV (percent).

Hard rule: long wing contracts must always exceed short body contracts.
For fixed mode, `long_short_ratio > 1` (e.g. `fixed:1.3` → 13 longs per 10
shorts). `fixed:1` is rejected. Credit picks a real ratio in `(1, maxWingMult]`
that is a net credit after slippage and commission (no 2:1 floor); if none
exists the structure is not opened. Theta sizes from at least `n+1` upward.

Shrink or pin any dimension without rebuilding (comma lists; one value = fixed):

```bash
./build/w_search ... --short-dtes 30 --long-dtes 60,90 \
  --wings fixed:2 --roll-dtes 0 --profit-takes 0.75 --loss-cuts off
```

| Flag | Example |
|------|---------|
| `--short-dtes` | `30` or `5,14,30` |
| `--long-dtes` | `60,90` |
| `--short-deltas` / `--long-deltas` | `0.45` / `0.08,0.12` |
| `--wings` | `fixed:2,fixed:1.3,credit,theta` |
| `--roll-dtes` | `0` |
| `--profit-takes` / `--loss-cuts` | `off,0.75` (use `off` to disable) |
| `--hedge-ratios` | `0.5,1,1.5,2` (only with `--long-pos-mil`) |
| `--call-put-ratio-pcts` | `100,50,25` |

Wing modes:

- **fixed(ratio)**: buy `round(short_contracts × ratio)` wing contracts
  (`m_call` / `m_put` are long/short ratios, e.g. `1.3` → 13 longs per 10
  shorts; ratio must be `> 1`).
- **credit**: choose the largest long/short ratio in `(1, maxWingMult]` (e.g.
  `1.1`, `2.25`) such that long debit ≤ short credit × `(1 - creditBuffer)`
  **after slippage and commission**. Longs must still exceed shorts. If even
  the minimum (`short+1`) is a debit, skip the open.
- **theta**: increase wing size from `n+1` while net structure theta ≥
  `thetaThreshold` (never below `n+1`).

| Dataset | Days | Python | C++ | Speedup |
|---------|------|--------|-----|---------|
| `SPY_fixture_small.csv` | 8 | ~1310 ms | ~11–16 ms | **~80–120×** |
| `SPY_50000.csv` (first ~58 sessions) | 58 | ~9040 ms | ~41 ms | **~220×** |

Parity on the small fixture: GBS matches to ~1e-15; backtest `finalNLV` within ~$1 on a $100k book; `numRolls` identical.

## Notes

- C++ CLI defaults gamma scalp / expected P/L off (same as typical search configs).
- Use `--verbose` on `w_backtest` for per-leg trade CSVs, or `--verbose-log` for a foldable HTML trade diary. Search stays summary-only for speed unless `--verbose-log` is set (then the winner is re-run once).
- Weekly/daily DTE synthesis and wing modes (`fixed` / `credit` / `theta`) are implemented in the library; `w_search` sweeps them, `w_backtest` exposes fixed wings via `--m-call` / `--m-put`.
- MaxDrawdown is capped at 100% (account wiped out). Overlay mode no longer funds W rolls with a silent negative-cash loan: net cash from close+open is raised by selling highest-cost stock lots, or invested by buying more stock. Year-end tax uses short- vs long-term rates, sells stock to pay the bill, and carries unused losses forward.
