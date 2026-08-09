#!/usr/bin/env python3
"""Parallel SPX parameter search with weekly/daily DTE + dynamic wing modes.

Uses multiprocessing over the thinned 2018–2019 ATM cache, then evaluates the
best config's expected P/L by price-move ranges.
"""

from __future__ import annotations

import argparse
import csv
import multiprocessing as mp
import os
import sys
import time
import traceback
import warnings

import pandas as pd

from Param import FixedParam, WParam
from Positions import PositionsAndStats
from StrategyW import w_strategy, open_w_structure
from ExpectedPnL import (
    expected_pnl_by_ranges, format_expected_pnl_ranges, DEFAULT_MOVE_RANGES,
)
from PriceDataUtil import Monthly_fields, load_yield_data
from WBacktest import _NullDictWriter, resolve_yield_path
from run_spx_search import ensure_cache, score_result


CACHE_DIR = os.path.join('data', 'cache')

# Worker globals (set by pool initializer)
_W_DF = None
_W_FIXED = None


def _param_to_kwargs(param: WParam) -> dict:
    return {
        'short_dte': param.short_dte,
        'long_dte': param.long_dte,
        'short_delta': param.short_delta,
        'long_delta': param.long_delta,
        'n_call': param.n_call,
        'n_put': param.n_put,
        'm_call': param.m_call,
        'm_put': param.m_put,
        'units': param.units,
        'wing_mode': param.wing_mode,
        'credit_buffer': param.credit_buffer,
        'theta_threshold': param.theta_threshold,
        'max_wing_mult': param.max_wing_mult,
        'roll_dte': param.roll_dte,
        'roll_on_expiry': param.roll_on_expiry,
        'profit_take_pct': param.profit_take_pct * 100,
        'loss_cut_pct': param.loss_cut_pct * 100,
        'enable_profit_take': param.enable_profit_take,
        'enable_loss_cut': param.enable_loss_cut,
        'enable_dte_roll': param.enable_dte_roll,
        'enable_gamma_scalp': param.enable_gamma_scalp,
        'gamma_scalp_delta_thresh': param.gamma_scalp_delta_thresh,
        'gamma_scalp_stop_delta': param.gamma_scalp_stop_delta,
        'eval_expected_pnl': False,
    }


def _init_worker(cache_path, yield_path, balance, symbol):
    global _W_DF, _W_FIXED
    warnings.filterwarnings('ignore', category=RuntimeWarning, module=r'optlib\.gbs')
    load_yield_data(yield_path)
    _W_DF = pd.read_pickle(cache_path)
    fixed = FixedParam(initial_balance=balance, symbol=symbol)
    fixed.summary_writer = _NullDictWriter()
    fixed.monthly_writer = _NullDictWriter(Monthly_fields)
    _W_FIXED = fixed


def _run_one(kwargs):
    try:
        param = WParam(fixed_param=_W_FIXED, **kwargs)
        t0 = time.time()
        result = w_strategy(
            _W_DF, param, debug=False, PositionsAndStats=PositionsAndStats)
        score = score_result(result)
        return {
            'ok': True,
            'kwargs': kwargs,
            'label': param.label(),
            'annualReturn': float(result['annualReturn']),
            'maxDrawdown': float(result['maxDrawdown']),
            'score': float(score),
            'totalOptGain': float(
                result['stat'].total_put_capital_gain
                + result['stat'].total_call_capital_gain),
            'totalStkGain': float(result['stat'].total_stock_capital_gain),
            'finalNLV': float(result['finalNLV']),
            'numRolls': int(result['numRolls']),
            'numGammaHedges': int(result['numGammaHedges']),
            'elapsed': time.time() - t0,
        }
    except Exception as exc:
        return {
            'ok': False,
            'kwargs': kwargs,
            'error': f'{exc}\n{traceback.format_exc()}',
        }


def build_search_params(fixed, units):
    """Grid covering daily/weekly/monthly DTEs and wing sizing modes."""
    # Daily(5), weekly(7/14), monthly(30/45) shorts; longer monthly wings
    short_dtes = [5, 7, 14, 30, 45]
    long_dtes = [30, 60, 90]
    short_deltas = [0.35, 0.45]
    long_deltas = [0.08, 0.12]
    wing_specs = [
        {'wing_mode': 'fixed', 'm_call': 2, 'm_put': 2},
        {'wing_mode': 'fixed', 'm_call': 3, 'm_put': 3},
        {'wing_mode': 'credit', 'm_call': 1, 'm_put': 1,
         'credit_buffer': 0.02, 'max_wing_mult': 4},
        {'wing_mode': 'theta', 'm_call': 1, 'm_put': 1,
         'theta_threshold': 0.0, 'max_wing_mult': 4},
    ]

    params = []
    for sd in short_dtes:
        for ld in long_dtes:
            if ld < sd:
                continue
            for sdelta in short_deltas:
                for ldelta in long_deltas:
                    if not (0 < ldelta < sdelta < 1):
                        continue
                    for wing in wing_specs:
                        params.append(WParam(
                            fixed_param=fixed,
                            short_dte=sd,
                            long_dte=ld,
                            short_delta=sdelta,
                            long_delta=ldelta,
                            n_call=1, n_put=1,
                            m_call=wing['m_call'],
                            m_put=wing['m_put'],
                            units=units,
                            wing_mode=wing['wing_mode'],
                            credit_buffer=wing.get('credit_buffer', 0.02),
                            theta_threshold=wing.get('theta_threshold', 0.0),
                            max_wing_mult=wing.get('max_wing_mult', 4),
                            roll_dte=0,
                            roll_on_expiry=True,
                            profit_take_pct=50,
                            loss_cut_pct=100,
                            enable_profit_take=True,
                            enable_loss_cut=True,
                            enable_dte_roll=True,
                            enable_gamma_scalp=False,
                            gamma_scalp_delta_thresh=0.15,
                            eval_expected_pnl=False,
                        ))
    return params


def write_results_csv(path, rows):
    fieldnames = [
        'shortDTE', 'longDTE', 'shortDelta', 'longDelta',
        'wingMode', 'mCall', 'mPut', 'creditBuffer', 'thetaThreshold',
        'maxWingMult', 'rollDTE', 'gammaScalp',
        'annualReturn', 'maxDrawdown', 'score',
        'totalOptGain', 'totalStkGain', 'finalNLV',
        'numRolls', 'numGammaHedges', 'elapsedSec',
    ]
    with open(path, 'w', encoding='UTF8', newline='') as fout:
        writer = csv.DictWriter(fout, fieldnames=fieldnames)
        writer.writeheader()
        for r in rows:
            kw = r['kwargs']
            writer.writerow({
                'shortDTE': kw['short_dte'],
                'longDTE': kw['long_dte'],
                'shortDelta': kw['short_delta'],
                'longDelta': kw['long_delta'],
                'wingMode': kw['wing_mode'],
                'mCall': kw['m_call'],
                'mPut': kw['m_put'],
                'creditBuffer': kw.get('credit_buffer', ''),
                'thetaThreshold': kw.get('theta_threshold', ''),
                'maxWingMult': kw.get('max_wing_mult', ''),
                'rollDTE': kw['roll_dte'],
                'gammaScalp': int(kw['enable_gamma_scalp']),
                'annualReturn': round(r['annualReturn'], 4),
                'maxDrawdown': round(r['maxDrawdown'], 4),
                'score': round(r['score'], 4),
                'totalOptGain': round(r['totalOptGain'], 2),
                'totalStkGain': round(r['totalStkGain'], 2),
                'finalNLV': round(r['finalNLV'], 2),
                'numRolls': r['numRolls'],
                'numGammaHedges': r['numGammaHedges'],
                'elapsedSec': round(r['elapsed'], 1),
            })


def evaluate_expected_ranges(df, fixed, best_kwargs, horizon_days=5):
    """Open one structure on mid sample and print E[PnL] by move range."""
    param = WParam(fixed_param=fixed, **{**best_kwargs, 'eval_expected_pnl': False})
    dates = sorted(df['DataDate'].unique())
    mid = dates[len(dates) // 2]
    # Prefer a liquid mid-year day
    for d in dates:
        if pd.Timestamp(d).month == 6:
            mid = d
            break
    group = df[df['DataDate'] == mid]
    stat = PositionsAndStats()
    stat.all_data = df
    ok = open_w_structure(param, pd.Timestamp(mid), group, stat, debug=False)
    if not ok or not stat.opt_list:
        print('Could not open structure for E[PnL] range analysis', file=sys.stderr)
        return None

    spots = (
        df.groupby('DataDate')['UnderlyingPrice'].first()
        .sort_index().astype(float).tolist()
    )
    spot = float(group['UnderlyingPrice'].iloc[0])
    expected, rows, _pnls, _probs = expected_pnl_by_ranges(
        stat.opt_list, spot, pd.Timestamp(mid), stat.structure_entry_cash,
        ranges=DEFAULT_MOVE_RANGES,
        spots_history=spots,
        horizon_days=horizon_days,
        use_empirical=len(spots) > 40,
    )
    print('\n=== Expected P/L by price-move range (best params) ===', file=sys.stderr)
    print(f'Date={pd.Timestamp(mid).date()} spot={spot:.2f} '
          f'M={stat.last_wing_m} mode={param.wing_mode}', file=sys.stderr)
    print(format_expected_pnl_ranges(rows, expected_total=expected), file=sys.stderr)
    return expected, rows


def main():
    warnings.filterwarnings('ignore', category=RuntimeWarning, module=r'optlib\.gbs')
    parser = argparse.ArgumentParser(description='Parallel SPX W search v2')
    parser.add_argument('-i', '--input-path', default='../options-data/SPX.csv')
    parser.add_argument('-y', '--yield-file', default='data/yield-curve-rates-1990-2024.csv')
    parser.add_argument('--units', type=int, default=10)
    parser.add_argument('--balance', type=float, default=1_000_000)
    parser.add_argument('--workers', type=int, default=0,
                        help='Parallel workers (0 = cpu_count)')
    parser.add_argument('--stage', choices=['train', 'holdout', 'all'], default='all')
    parser.add_argument('--limit', type=int, default=0,
                        help='Limit configs (debug); 0 = all')
    args = parser.parse_args()

    workers = args.workers or max(1, os.cpu_count() or 4)
    yield_path = resolve_yield_path(args.yield_file)
    load_yield_data(yield_path)
    fixed = FixedParam(initial_balance=args.balance, symbol='SPX')
    fixed.summary_writer = _NullDictWriter()
    fixed.monthly_writer = _NullDictWriter(Monthly_fields)

    train_cache = os.path.join(CACHE_DIR, 'spx_2018_2019_e5_atm.pkl')
    holdout_cache = os.path.join(CACHE_DIR, 'spx_2020_2021_e5_atm.pkl')

    best_kwargs = None
    if args.stage in ('train', 'all'):
        ensure_cache(args.input_path, '2018-01-01', '2019-12-31', 5, train_cache)
        params = build_search_params(fixed, args.units)
        if args.limit > 0:
            params = params[:args.limit]
        jobs = [_param_to_kwargs(p) for p in params]
        print(f'\n=== TRAIN 2018-2019: {len(jobs)} configs, {workers} workers ===',
              file=sys.stderr)

        t0 = time.time()
        results = []
        ctx = mp.get_context('spawn')
        with ctx.Pool(
            processes=workers,
            initializer=_init_worker,
            initargs=(train_cache, yield_path, args.balance, 'SPX'),
        ) as pool:
            for i, row in enumerate(pool.imap_unordered(_run_one, jobs), 1):
                if not row.get('ok'):
                    print(f'[{i}/{len(jobs)}] FAIL {row.get("kwargs")}\n{row.get("error")}',
                          file=sys.stderr)
                    continue
                results.append(row)
                print(f'[{i}/{len(jobs)}] score={row["score"]:.2f} '
                      f'ret={row["annualReturn"]:.2f}% dd={row["maxDrawdown"]:.2f}% '
                      f'({row["elapsed"]:.0f}s) | {row["label"]}',
                      file=sys.stderr)

        ranked = sorted(results, key=lambda x: -x['score'])
        write_results_csv('spx-search-v2-train.csv', ranked)
        print(f'TRAIN done in {time.time()-t0:.0f}s  wrote spx-search-v2-train.csv',
              file=sys.stderr)
        print('\n=== TRAIN top 10 ===', file=sys.stderr)
        for r in ranked[:10]:
            print(f'  score={r["score"]:.2f} ret={r["annualReturn"]:.2f}% '
                  f'dd={r["maxDrawdown"]:.2f}% | {r["label"]}', file=sys.stderr)
        if not ranked:
            raise SystemExit('No successful train runs')
        best_kwargs = ranked[0]['kwargs']
        best_label = ranked[0]['label']

        # Expected P/L ranges on training data for best params
        df_train = pd.read_pickle(train_cache)
        evaluate_expected_ranges(df_train, fixed, best_kwargs)

    if args.stage in ('holdout', 'all'):
        if best_kwargs is None:
            # Load best from train CSV
            if not os.path.exists('spx-search-v2-train.csv'):
                raise SystemExit('Run train stage first')
            top = pd.read_csv('spx-search-v2-train.csv').iloc[0]
            best_kwargs = {
                'short_dte': int(top['shortDTE']),
                'long_dte': int(top['longDTE']),
                'short_delta': float(top['shortDelta']),
                'long_delta': float(top['longDelta']),
                'n_call': 1, 'n_put': 1,
                'm_call': int(top['mCall']),
                'm_put': int(top['mPut']),
                'units': args.units,
                'wing_mode': str(top['wingMode']),
                'credit_buffer': float(top['creditBuffer'] or 0.02),
                'theta_threshold': float(top['thetaThreshold'] or 0),
                'max_wing_mult': int(top['maxWingMult'] or 4),
                'roll_dte': int(top['rollDTE']),
                'roll_on_expiry': True,
                'profit_take_pct': 50,
                'loss_cut_pct': 100,
                'enable_profit_take': True,
                'enable_loss_cut': True,
                'enable_dte_roll': True,
                'enable_gamma_scalp': bool(int(top['gammaScalp'])),
                'gamma_scalp_delta_thresh': 0.15,
                'gamma_scalp_stop_delta': 0.05,
                'eval_expected_pnl': False,
            }
            best_label = WParam(fixed_param=fixed, **best_kwargs).label()

        ensure_cache(args.input_path, '2020-01-01', '2021-12-31', 5, holdout_cache)
        print(f'\n=== HOLDOUT 2020-2021 best: {best_label} ===', file=sys.stderr)
        _init_worker(holdout_cache, yield_path, args.balance, 'SPX')
        hold = _run_one(best_kwargs)
        if not hold.get('ok'):
            raise SystemExit(hold.get('error'))
        write_results_csv('spx-search-v2-holdout.csv', [hold])
        print(f'HOLDOUT ret={hold["annualReturn"]:.2f}% dd={hold["maxDrawdown"]:.2f}% '
              f'score={hold["score"]:.2f}', file=sys.stderr)

        df_hold = pd.read_pickle(holdout_cache)
        evaluate_expected_ranges(df_hold, fixed, best_kwargs)

    print('\n=== BEST PARAMETERS ===', file=sys.stderr)
    for k, v in best_kwargs.items():
        print(f'  {k}={v}', file=sys.stderr)


if __name__ == '__main__':
    # Required on macOS for spawn
    mp.set_start_method('spawn', force=True)
    main()
