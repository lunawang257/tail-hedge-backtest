# How to install

## Checkout submodule "optlib"

```bash
git submodule init
git submodule update
```

## Setup python env

```bash
python3 -m venv env
source env/bin/activate
pip install --upgrade pip
pip install -r requirements.txt
cd optlib && python setup.py install && cd ..
```

Bundled sample data lives under `data/` (`SPY_50000.csv` + yield curve). The
project is self-contained and does not require sibling repos at runtime.

# Overview

`WBacktest.py` backtests a **W** tail-hedge options strategy whose profit/loss
curve vs the underlying looks like the letter **W**.

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
| Profit take | Close when structure P/L ≥ `--profit-take-pct` of \|entry cash\| |
| Loss cut | Close when structure P/L ≤ `-loss-cut-pct` (avoid futile gamma scalping) |
| Gamma scalping | When book θ < 0, hedge stock to Δ≈0 once \|Δ\| exceeds threshold |

## Expected P/L

Periodically marks `E[PnL] = Σ PnL(move_i) · P(move_i)` over a move grid, with
lognormal probabilities from realized vol (or empirical returns when enough
history exists).

# Usage

```bash
# Default: bundled SPY sample under data/
./WBacktest.py -d

# Explicit local paths
./WBacktest.py -i data/SPY_50000.csv -y data/yield-curve-rates-1990-2024.csv

# Parameter sweep (bring your own larger history CSV)
./WBacktest.py -i data/SPY.csv \
  --short-dte 21,35,7 --long-dte 45,90,15 \
  --short-delta 0.35,0.45,0.05 --long-delta 0.08,0.12,0.02 \
  --roll-dte 5,10,5 --gamma-scalp-delta-thresh 0.10,0.20,0.05 \
  -o sweep-details.csv -s sweep-summary.csv
```

## Key arguments

| Arg | Default | Meaning |
|-----|---------|---------|
| `--symbol` | `SPY` | Underlying |
| `-i` | `data/SPY_50000.csv` | Options CSV |
| `-y` | `data/yield-curve-rates-1990-2024.csv` | Yield curve |
| `--short-dte` / `--long-dte` | 30 / 60 | Tenors (supports `start,stop,step`) |
| `--short-delta` / `--long-delta` | 0.40 / 0.10 | Absolute deltas |
| `--n-call` `--n-put` | 1 | Short contracts per unit |
| `--m-call` `--m-put` | 2 | Long contracts per unit |
| `--units` | 1 | Scale all contract counts |
| `--roll-dte` | 7 | DTE roll trigger |
| `--profit-take-pct` | 50 | Profit-take threshold |
| `--loss-cut-pct` | 100 | Loss-cut threshold |
| `--gamma-scalp-delta-thresh` | 0.15 | Start hedge when \|Δ\| / scale exceeds this |
| `--no-gamma-scalp` | off | Disable dynamic hedging |
| `--no-profit-take` / `--no-loss-cut` / `--no-dte-roll` | off | Disable individual rules |

# Code layout

```
WBacktest.py          CLI + parameter sweep
StrategyW.py          Open / close / roll / gamma-scalp loop
Param.py              FixedParam, WParam, range expansion
Positions.py          PositionsAndStats, DayOptionLookup
ChainInterpolate.py   Odd-DTE IV interpolation + delta strike picker
ExpectedPnL.py        Move × probability expected P/L
OptionPos.py          Option lots + Greeks
StockPos.py           Stock lots (hedges)
PriceDataUtil.py      Yields, BS pricing, commissions, CSV writers
Util.py               Date / rounding helpers
data/                 Bundled SPY sample + yield curve
optlib/               Merton / IV (git submodule)
```

# Outputs

- **Detail CSV** (`-o`): every open/close/hedge/mark with cost basis, gains,
  commissions, structure P/L, net Greeks, expected P/L, drawdown.
- **Summary CSV** (`-s`): one row per parameter set with annualized return,
  max drawdown, tax/commission rates, roll & hedge counts.
