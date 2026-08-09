#!/usr/bin/env python3
"""CLI for the W tail-hedge options backtest.

Default underlying: SPY. Sweeps parameter ranges and writes detail + summary CSV.
"""

from __future__ import annotations

import argparse
import csv
import os
import sys
import time
import warnings

import pandas as pd

from Param import FixedParam, expand_w_params
from Positions import PositionsAndStats
from StrategyW import w_strategy
from Util import round_float
from PriceDataUtil import (
    Summary_fields, Monthly_fields,
    load_yield_data, adjust_prices_for_splits_and_merges,
)


Debug = False


def default_log_path(output_path):
    base, ext = os.path.splitext(output_path)
    return f'{base}.log' if ext else f'{output_path}.log'


def setup_run_log(log_path):
    log_stream = open(log_path, 'a', encoding='utf-8', buffering=1)
    sys.stderr = log_stream
    return log_stream


def load_options_csv(path, start_date=None, end_date=None, trade_every_n_days=1):
    """Load options CSV, optionally filtering by date and thinning sessions.

    Large files (e.g. SPX.csv) are read in chunks so date filters apply before
    the full frame is materialised.
    """
    print(f'Loading {path} ...', file=sys.stderr)
    start_ts = pd.Timestamp(start_date).normalize() if start_date else None
    end_ts = pd.Timestamp(end_date).normalize() if end_date else None

    usecols = [
        'Symbol', 'ExpirationDate', 'AskPrice', 'BidPrice', 'LastPrice',
        'PutCall', 'StrikePrice', 'UnderlyingPrice', 'DataDate',
    ]
    reader_kwargs = {'usecols': usecols, 'chunksize': 500_000}
    if path.endswith('.gz'):
        reader_kwargs['compression'] = 'gzip'

    chunks = []
    total_rows = 0
    for chunk in pd.read_csv(path, **reader_kwargs):
        chunk['DataDate'] = pd.to_datetime(chunk['DataDate'])
        if start_ts is not None:
            chunk = chunk[chunk['DataDate'] >= start_ts]
        if end_ts is not None:
            chunk = chunk[chunk['DataDate'] <= end_ts]
        if chunk.empty:
            continue
        total_rows += len(chunk)
        chunks.append(chunk)

    if not chunks:
        raise ValueError(
            f'No rows loaded from {path} for date filter {start_date}..{end_date}')

    df = pd.concat(chunks, ignore_index=True)
    df['ExpirationDate'] = pd.to_datetime(df['ExpirationDate'])
    df['PutCall'] = df['PutCall'].str.lower().str[0]
    df = adjust_prices_for_splits_and_merges(df)

    if trade_every_n_days and trade_every_n_days > 1:
        dates = sorted(df['DataDate'].dt.normalize().unique())
        keep = set(dates[::trade_every_n_days])
        keep.add(dates[0])
        keep.add(dates[-1])
        before = df['DataDate'].nunique()
        df = df[df['DataDate'].dt.normalize().isin(keep)].copy()
        print(f'  thinned sessions {before} → {df["DataDate"].nunique()} '
              f'(every {trade_every_n_days} days)', file=sys.stderr)

    print(f'  rows={len(df)} days={df["DataDate"].nunique()} '
          f'{df["DataDate"].min().date()} → {df["DataDate"].max().date()} '
          f'(filtered rows {total_rows})',
          file=sys.stderr)
    return df


def resolve_input_path(path, symbol):
    """Resolve input path; prefer bundled data/ files."""
    if path:
        return path
    candidates = [
        os.path.join('data', f'{symbol}_50000.csv'),
        os.path.join('data', f'{symbol}.csv'),
        os.path.join('data', f'{symbol}_200k.csv'),
        f'{symbol}_50000.csv',
        f'{symbol}.csv',
    ]
    for c in candidates:
        if os.path.exists(c):
            return c
    raise FileNotFoundError(
        f'No input data found for {symbol}. Place a CSV under data/ '
        f'(e.g. data/{symbol}_50000.csv) or pass -i. Tried: {candidates}')


def resolve_yield_path(path):
    """Resolve treasury yield file; prefer bundled data/ copy."""
    candidates = [
        path,
        os.path.join('data', 'yield-curve-rates-1990-2024.csv'),
        'yield-curve-rates-1990-2024.csv',
    ]
    for c in candidates:
        if c and os.path.exists(c):
            return c
    raise FileNotFoundError(
        'Yield curve file not found. Expected data/yield-curve-rates-1990-2024.csv '
        f'or pass -y. Tried: {candidates}')


class _NullDictWriter:
    """No-op CSV writer used for --summary-only sweeps."""

    def __init__(self, fieldnames=None):
        self.fieldnames = fieldnames or []

    def writeheader(self):
        return None

    def writerow(self, row):
        return None


def main():
    global Debug
    parser = argparse.ArgumentParser(
        description='Backtest the W tail-hedge options strategy')

    parser.add_argument('-i', '--input-path', default='',
                        help='Options CSV (default: auto-resolve SPY data)')
    parser.add_argument('--symbol', default='SPY',
                        help='Underlying symbol (default: SPY)')
    parser.add_argument('-o', '--output-path', default='w-details.csv',
                        help='Detail trade log CSV')
    parser.add_argument('-s', '--summary-output-path', default='w-summary.csv',
                        help='Summary CSV per parameter set')
    parser.add_argument('--log-path', default='',
                        help='Log path (default: derived from -o)')
    parser.add_argument('-y', '--yield-file',
                        default='data/yield-curve-rates-1990-2024.csv',
                        help='Treasury yield curve CSV')
    parser.add_argument('-d', '--debug', action='store_true')
    parser.add_argument('--balance', type=float, default=1_000_000,
                        help='Initial cash balance')
    parser.add_argument('--start-date', default='',
                        help='Inclusive start date YYYY-MM-DD (optional)')
    parser.add_argument('--end-date', default='',
                        help='Inclusive end date YYYY-MM-DD (optional)')
    parser.add_argument('--trade-every-n-days', type=int, default=1,
                        help='Keep every Nth trading session (1=all days)')

    # W structure
    parser.add_argument('--short-dte', default='30',
                        help="Short-leg DTE: v or start,stop,step")
    parser.add_argument('--long-dte', default='60',
                        help="Long-leg DTE: v or start,stop,step")
    parser.add_argument('--short-delta', default='0.40',
                        help='|delta| for short legs')
    parser.add_argument('--long-delta', default='0.10',
                        help='|delta| for long legs')
    parser.add_argument('--n-call', type=int, default=1)
    parser.add_argument('--n-put', type=int, default=1)
    parser.add_argument('--m-call', default='2',
                        help='Long call contracts (fixed mode): v or start,stop,step')
    parser.add_argument('--m-put', default='2',
                        help='Long put contracts (fixed mode): v or start,stop,step')
    parser.add_argument('--units', type=int, default=1,
                        help='Number of W units (scales all contract counts)')
    parser.add_argument('--wing-mode', default='fixed',
                        help='Wing sizing: fixed,credit,theta (comma-separated for sweep)')
    parser.add_argument('--credit-buffer', default='0.02',
                        help='For credit mode: longs cost <= (1-buffer)*short premium')
    parser.add_argument('--theta-threshold', default='0',
                        help='For theta mode: keep net θ >= this ($/day per unit)')
    parser.add_argument('--max-wing-mult', default='5',
                        help='Cap M <= max_wing_mult * N for dynamic wing modes')

    # Roll / close rules
    parser.add_argument('--roll-dte', default='7',
                        help='Roll when short DTE <= this')
    parser.add_argument('--no-roll-on-expiry', action='store_true')
    parser.add_argument('--profit-take-pct', type=float, default=50)
    parser.add_argument('--loss-cut-pct', type=float, default=100)
    parser.add_argument('--no-profit-take', action='store_true')
    parser.add_argument('--no-loss-cut', action='store_true')
    parser.add_argument('--no-dte-roll', action='store_true')

    # Gamma scalping
    parser.add_argument('--gamma-scalp-delta-thresh', default='0.15',
                        help='|Δ| fraction threshold to hedge')
    parser.add_argument('--gamma-scalp-stop-delta', type=float, default=0.05)
    parser.add_argument('--no-gamma-scalp', action='store_true')
    parser.add_argument('--no-expected-pnl', action='store_true')
    parser.add_argument('--summary-only', action='store_true',
                        help='Skip writing detail trade rows (faster sweeps)')

    args = parser.parse_args()
    Debug = args.debug

    log_path = args.log_path or default_log_path(args.output_path)
    log_stream = setup_run_log(log_path)
    warnings.filterwarnings('ignore', category=RuntimeWarning, module=r'optlib\.gbs')

    input_path = resolve_input_path(args.input_path, args.symbol.upper())
    yield_path = resolve_yield_path(args.yield_file)
    load_yield_data(yield_path)

    print(f'Output: {args.output_path} {args.summary_output_path}', file=sys.stderr)
    print(f'Log: {log_path}', file=sys.stderr)

    if args.summary_only:
        detail_writer = _NullDictWriter(Monthly_fields)
        fout_detail = None
    else:
        fout_detail = open(args.output_path, 'w', encoding='UTF8', newline='')
        detail_writer = csv.DictWriter(fout_detail, fieldnames=Monthly_fields)
        detail_writer.writeheader()

    fout_sum = open(args.summary_output_path, 'w', encoding='UTF8', newline='')
    sum_writer = csv.DictWriter(fout_sum, fieldnames=Summary_fields)
    sum_writer.writeheader()

    fixed = FixedParam(
        initial_balance=args.balance,
        monthly_writer=detail_writer,
        summary_writer=sum_writer,
        symbol=args.symbol.upper(),
    )

    args_dict = {
        'short_dte': args.short_dte,
        'long_dte': args.long_dte,
        'short_delta': args.short_delta,
        'long_delta': args.long_delta,
        'n_call': args.n_call,
        'n_put': args.n_put,
        'm_call': args.m_call,
        'm_put': args.m_put,
        'units': args.units,
        'wing_mode': args.wing_mode,
        'credit_buffer': args.credit_buffer,
        'theta_threshold': args.theta_threshold,
        'max_wing_mult': args.max_wing_mult,
        'roll_dte': args.roll_dte,
        'roll_on_expiry': not args.no_roll_on_expiry,
        'profit_take_pct': args.profit_take_pct,
        'loss_cut_pct': args.loss_cut_pct,
        'enable_profit_take': not args.no_profit_take,
        'enable_loss_cut': not args.no_loss_cut,
        'enable_dte_roll': not args.no_dte_roll,
        'enable_gamma_scalp': not args.no_gamma_scalp,
        'gamma_scalp_delta_thresh': args.gamma_scalp_delta_thresh,
        'gamma_scalp_stop_delta': args.gamma_scalp_stop_delta,
        'eval_expected_pnl': not args.no_expected_pnl,
    }
    params = expand_w_params(fixed, args_dict)
    if not params:
        print('No valid parameter combinations', file=sys.stderr)
        sys.exit(1)
    print(f'Parameter sets: {len(params)}', file=sys.stderr)

    df = load_options_csv(
        input_path,
        start_date=args.start_date or None,
        end_date=args.end_date or None,
        trade_every_n_days=max(1, args.trade_every_n_days),
    )
    t0 = time.time()
    results = []
    for i, param in enumerate(params):
        print(f'[{i+1}/{len(params)}] {param.label()}', file=sys.stderr)
        result = w_strategy(df, param, debug=Debug, PositionsAndStats=PositionsAndStats)
        results.append((param, result))
        print(f'  annualReturn={round_float(result["annualReturn"])}% '
              f'maxDD={round_float(result["maxDrawdown"])}% '
              f'rolls={result["numRolls"]} hedges={result["numGammaHedges"]} '
              f'E[PnL]={round_float(result["expectedPnL"])}',
              file=sys.stderr)

    # Rank by annual return then drawdown
    ranked = sorted(
        results,
        key=lambda x: (-x[1]['annualReturn'], x[1]['maxDrawdown']))
    print('\n=== Top configurations ===', file=sys.stderr)
    for param, result in ranked[:10]:
        print(f'  {param.label()}  →  '
              f'ret={round_float(result["annualReturn"])}% '
              f'dd={round_float(result["maxDrawdown"])}% '
              f'E[PnL]={round_float(result["expectedPnL"])}',
              file=sys.stderr)

    print(f'Done in {time.time() - t0:.1f}s', file=sys.stderr)
    if fout_detail is not None:
        fout_detail.close()
    fout_sum.close()
    log_stream.close()


if __name__ == '__main__':
    main()
