#!/usr/bin/env python3
"""Multi-stage SPX parameter search for the W strategy.

Builds a local parquet cache from ../options-data/SPX.csv, then runs coarse →
refine → holdout sweeps. Scores by risk-adjusted return:
  score = annualReturn - 0.5 * maxDrawdown
"""

from __future__ import annotations

import argparse
import csv
import os
import sys
import time
import warnings

import pandas as pd

from Param import FixedParam, WParam, expand_w_params
from Positions import PositionsAndStats
from StrategyW import w_strategy
from Util import round_float
from PriceDataUtil import (
    Monthly_fields, load_yield_data, adjust_prices_for_splits_and_merges,
)
from WBacktest import load_options_csv, _NullDictWriter, resolve_yield_path


CACHE_DIR = os.path.join('data', 'cache')


def ensure_cache(spx_path, start, end, every_n, cache_path, atm_band=0.20):
    """Load/build a session-thinned cache, optionally strike-filtered around spot."""
    if os.path.exists(cache_path):
        print(f'Using cache {cache_path}', file=sys.stderr)
        return pd.read_pickle(cache_path)

    os.makedirs(os.path.dirname(cache_path), exist_ok=True)
    raw_path = cache_path.replace('.pkl', '_raw.pkl')
    if os.path.exists(raw_path):
        df = pd.read_pickle(raw_path)
    else:
        df = load_options_csv(
            spx_path, start_date=start, end_date=end, trade_every_n_days=every_n)
        df.to_pickle(raw_path)
        print(f'Wrote raw cache {raw_path} rows={len(df)}', file=sys.stderr)

    if atm_band is not None and atm_band > 0:
        spot = df.groupby('DataDate')['UnderlyingPrice'].transform('first')
        lo, hi = 1.0 - atm_band, 1.0 + atm_band
        df = df[(df['StrikePrice'] >= spot * lo) & (df['StrikePrice'] <= spot * hi)].copy()

    df['DataDate'] = pd.to_datetime(df['DataDate']).dt.normalize()
    df['ExpirationDate'] = pd.to_datetime(df['ExpirationDate']).dt.normalize()
    df.to_pickle(cache_path)
    print(f'Wrote cache {cache_path} rows={len(df)} days={df["DataDate"].nunique()}',
          file=sys.stderr)
    return df


def score_result(result):
    return result['annualReturn'] - 0.5 * result['maxDrawdown']


def run_grid(df, fixed, grid_kwargs, label, summary_path):
    args_dict = {
        'n_call': 1, 'n_put': 1, 'm_call': 2, 'm_put': 2,
        'roll_on_expiry': True,
        'profit_take_pct': 50,
        'loss_cut_pct': 100,
        'enable_profit_take': True,
        'enable_loss_cut': True,
        'enable_dte_roll': True,
        'enable_gamma_scalp': True,
        'gamma_scalp_stop_delta': 0.05,
        'eval_expected_pnl': False,
        **grid_kwargs,
    }
    # Expand gamma on/off as separate runs if requested
    gamma_modes = grid_kwargs.pop('_gamma_modes', None)
    if '_gamma_modes' in args_dict:
        del args_dict['_gamma_modes']

    params = expand_w_params(fixed, args_dict)
    if gamma_modes:
        extra = []
        for p in params:
            for enable in gamma_modes:
                kw = dict(p.__dict__)
                fp = kw.pop('fixed_param')
                # rebuild cleanly
                extra.append(WParam(
                    fixed_param=fp,
                    short_dte=p.short_dte,
                    long_dte=p.long_dte,
                    short_delta=p.short_delta,
                    long_delta=p.long_delta,
                    n_call=p.n_call, n_put=p.n_put,
                    m_call=p.m_call, m_put=p.m_put,
                    units=p.units,
                    roll_dte=p.roll_dte,
                    roll_on_expiry=p.roll_on_expiry,
                    profit_take_pct=p.profit_take_pct * 100,
                    loss_cut_pct=p.loss_cut_pct * 100,
                    enable_profit_take=p.enable_profit_take,
                    enable_loss_cut=p.enable_loss_cut,
                    enable_dte_roll=p.enable_dte_roll,
                    enable_gamma_scalp=enable,
                    gamma_scalp_delta_thresh=p.gamma_scalp_delta_thresh,
                    gamma_scalp_stop_delta=p.gamma_scalp_stop_delta,
                    eval_expected_pnl=False,
                ))
        params = extra

    print(f'\n=== {label}: {len(params)} configs ===', file=sys.stderr)
    fixed.summary_writer = _NullDictWriter()
    fixed.monthly_writer = _NullDictWriter(Monthly_fields)

    results = []
    t0 = time.time()
    for i, param in enumerate(params):
        param.fixed_param = fixed
        print(f'[{i+1}/{len(params)}] {param.label()}', file=sys.stderr)
        t1 = time.time()
        result = w_strategy(df, param, debug=False, PositionsAndStats=PositionsAndStats)
        result['score'] = score_result(result)
        results.append((param, result))
        print(f'  ret={round_float(result["annualReturn"])}% '
              f'dd={round_float(result["maxDrawdown"])}% '
              f'score={round_float(result["score"])} '
              f'optGain={round_float(result["stat"].total_put_capital_gain + result["stat"].total_call_capital_gain)} '
              f'stkGain={round_float(result["stat"].total_stock_capital_gain)} '
              f'rolls={result["numRolls"]} hedges={result["numGammaHedges"]} '
              f'({time.time()-t1:.0f}s)',
              file=sys.stderr)

    ranked = sorted(results, key=lambda x: -x[1]['score'])
    with open(summary_path, 'w', encoding='UTF8', newline='') as fout:
        writer = csv.DictWriter(
            fout,
            fieldnames=[
                'shortDTE', 'longDTE', 'shortDelta', 'longDelta',
                'rollDTE', 'gammaScalp', 'gammaDeltaThresh',
                'profitTake', 'lossCut', 'units',
                'annualReturn', 'maxDrawdown', 'score',
                'totalOptGain', 'totalStkGain', 'finalNLV',
                'numRolls', 'numGammaHedges',
            ])
        writer.writeheader()
        ranked = sorted(results, key=lambda x: -x[1]['score'])
        for param, result in ranked:
            writer.writerow({
                'shortDTE': param.short_dte,
                'longDTE': param.long_dte,
                'shortDelta': param.short_delta,
                'longDelta': param.long_delta,
                'rollDTE': param.roll_dte,
                'gammaScalp': int(param.enable_gamma_scalp),
                'gammaDeltaThresh': param.gamma_scalp_delta_thresh,
                'profitTake': int(param.enable_profit_take),
                'lossCut': int(param.enable_loss_cut),
                'units': param.units,
                'annualReturn': round(result['annualReturn'], 4),
                'maxDrawdown': round(result['maxDrawdown'], 4),
                'score': round(result['score'], 4),
                'totalOptGain': round(
                    result['stat'].total_put_capital_gain
                    + result['stat'].total_call_capital_gain, 2),
                'totalStkGain': round(result['stat'].total_stock_capital_gain, 2),
                'finalNLV': round(result['finalNLV'], 2),
                'numRolls': result['numRolls'],
                'numGammaHedges': result['numGammaHedges'],
            })

    print(f'{label} done in {time.time()-t0:.0f}s', file=sys.stderr)
    print(f'\n=== {label} top 10 ===', file=sys.stderr)
    for param, result in ranked[:10]:
        print(f'  score={result["score"]:.2f} ret={result["annualReturn"]:.2f}% '
              f'dd={result["maxDrawdown"]:.2f}% | {param.label()}',
              file=sys.stderr)
    return ranked


def main():
    warnings.filterwarnings('ignore', category=RuntimeWarning, module=r'optlib\.gbs')
    parser = argparse.ArgumentParser(description='SPX W-strategy parameter search')
    parser.add_argument('-i', '--input-path', default='../options-data/SPX.csv')
    parser.add_argument('-y', '--yield-file', default='data/yield-curve-rates-1990-2024.csv')
    parser.add_argument('--units', type=int, default=10,
                        help='W units (scale structure vs cash)')
    parser.add_argument('--balance', type=float, default=1_000_000)
    parser.add_argument('--stage', choices=['all', 'coarse', 'refine', 'rules', 'holdout'],
                        default='all')
    args = parser.parse_args()

    load_yield_data(resolve_yield_path(args.yield_file))
    fixed = FixedParam(initial_balance=args.balance, symbol='SPX')

    # Stage data windows
    coarse_cache = os.path.join(CACHE_DIR, 'spx_2018_2019_e5_atm.pkl')
    daily_cache = os.path.join(CACHE_DIR, 'spx_2016_2019_e1_atm.pkl')
    holdout_cache = os.path.join(CACHE_DIR, 'spx_2020_2021_e1_atm.pkl')

    ranked_coarse = None
    ranked_refine = None

    if args.stage in ('all', 'coarse'):
        df = ensure_cache(args.input_path, '2018-01-01', '2019-12-31', 5, coarse_cache)
        # Coarse screen: expire/profit/loss rolls only (no mid-tenor DTE roll),
        # no gamma scalping. Thinned sessions make small roll_dte fire every step.
        ranked_coarse = run_grid(
            df, fixed,
            {
                'short_dte': '21,45,24',          # 21, 45
                'long_dte': '60,90,30',            # 60, 90
                'short_delta': '0.35,0.45,0.10',   # 0.35, 0.45
                'long_delta': '0.08,0.12,0.04',    # 0.08, 0.12
                'roll_dte': '0',
                'gamma_scalp_delta_thresh': '0.15',
                'units': args.units,
                'enable_gamma_scalp': False,
            },
            'COARSE 2018-2019 (every 5d, expire-roll, no γ)',
            'spx-search-coarse.csv',
        )

    if args.stage in ('all', 'refine'):
        if ranked_coarse is None:
            # reload prior coarse ranking if present
            if not os.path.exists('spx-search-coarse.csv'):
                raise SystemExit('Run coarse stage first')
            # Build refine around typical winners: denser near top DTEs/deltas
            short_dtes = '21,35,7'
            long_dtes = '45,75,15'
            short_deltas = '0.30,0.45,0.05'
            long_deltas = '0.05,0.12,0.035'
        else:
            # Focus refine on neighborhoods of top-5 coarse winners
            tops = ranked_coarse[:5]
            short_dtes = sorted({p.short_dte for p, _ in tops})
            long_dtes = sorted({p.long_dte for p, _ in tops})
            short_deltas = sorted({round(p.short_delta, 2) for p, _ in tops})
            long_deltas = sorted({round(p.long_delta, 2) for p, _ in tops})
            # expand neighborhood
            def nbr_int(vals, step, lo, hi):
                out = set()
                for v in vals:
                    for d in (-step, 0, step):
                        nv = v + d
                        if lo <= nv <= hi:
                            out.add(nv)
                return sorted(out)
            def nbr_float(vals, step, lo, hi):
                out = set()
                for v in vals:
                    for d in (-step, 0, step):
                        nv = round(v + d, 3)
                        if lo < nv < hi:
                            out.add(nv)
                return sorted(out)
            short_dtes = nbr_int(short_dtes, 7, 14, 60)
            long_dtes = nbr_int(long_dtes, 15, 30, 120)
            short_deltas = nbr_float(short_deltas, 0.05, 0.2, 0.55)
            long_deltas = nbr_float(long_deltas, 0.03, 0.03, 0.2)
            short_dtes = ','.join(str(x) for x in short_dtes) if False else \
                f'{min(short_dtes)},{max(short_dtes)},7' if len(short_dtes) > 1 else str(short_dtes[0])
            # Use explicit comma lists for irregular grids via expand — parse_float_range only supports range.
            # So pick compact ranges covering neighborhoods:
            sd_list = nbr_int([p.short_dte for p, _ in tops], 7, 14, 60)
            ld_list = nbr_int([p.long_dte for p, _ in tops], 15, 30, 120)
            sdel_list = nbr_float([p.short_delta for p, _ in tops], 0.05, 0.2, 0.55)
            ldel_list = nbr_float([p.long_delta for p, _ in tops], 0.03, 0.03, 0.2)
            short_dtes = f'{min(sd_list)},{max(sd_list)},7'
            long_dtes = f'{min(ld_list)},{max(ld_list)},15'
            short_deltas = f'{min(sdel_list):.2f},{max(sdel_list):.2f},0.05'
            long_deltas = f'{min(ldel_list):.2f},{max(ldel_list):.2f},0.03'

        df = ensure_cache(args.input_path, '2016-01-01', '2019-12-31', 1, daily_cache)
        ranked_refine = run_grid(
            df, fixed,
            {
                'short_dte': short_dtes,
                'long_dte': long_dtes,
                'short_delta': short_deltas,
                'long_delta': long_deltas,
                'roll_dte': '5,14,3',
                'gamma_scalp_delta_thresh': '0.10,0.25,0.05',
                'units': args.units,
            },
            'REFINE 2016-2019 (daily)',
            'spx-search-refine.csv',
        )

    if args.stage in ('all', 'rules'):
        # Fix best structure from refine/coarse; sweep rule switches
        if ranked_refine:
            best = ranked_refine[0][0]
        elif ranked_coarse:
            best = ranked_coarse[0][0]
        else:
            raise SystemExit('Need coarse/refine before rules')

        df = ensure_cache(args.input_path, '2016-01-01', '2019-12-31', 1, daily_cache)
        # Manual rule grid
        rule_params = []
        for pt in (True, False):
            for lc in (True, False):
                for gs in (True, False):
                    for rd in (5, 7, 10, 14):
                        for gth in ([0.10, 0.15, 0.25] if gs else [0.15]):
                            rule_params.append(WParam(
                                fixed_param=fixed,
                                short_dte=best.short_dte,
                                long_dte=best.long_dte,
                                short_delta=best.short_delta,
                                long_delta=best.long_delta,
                                n_call=1, n_put=1, m_call=2, m_put=2,
                                units=args.units,
                                roll_dte=rd,
                                enable_profit_take=pt,
                                enable_loss_cut=lc,
                                enable_dte_roll=True,
                                enable_gamma_scalp=gs,
                                gamma_scalp_delta_thresh=gth,
                                profit_take_pct=50,
                                loss_cut_pct=100,
                                eval_expected_pnl=False,
                            ))
        print(f'\n=== RULES: {len(rule_params)} configs ===', file=sys.stderr)
        # reuse run_grid-like loop
        results = []
        t0 = time.time()
        for i, param in enumerate(rule_params):
            print(f'[{i+1}/{len(rule_params)}] {param.label()} '
                  f'pt={int(param.enable_profit_take)} lc={int(param.enable_loss_cut)}',
                  file=sys.stderr)
            result = w_strategy(df, param, debug=False, PositionsAndStats=PositionsAndStats)
            result['score'] = score_result(result)
            results.append((param, result))
            print(f'  score={result["score"]:.2f} ret={result["annualReturn"]:.2f}% '
                  f'dd={result["maxDrawdown"]:.2f}%', file=sys.stderr)
        ranked_rules = sorted(results, key=lambda x: -x[1]['score'])
        with open('spx-search-rules.csv', 'w', encoding='UTF8', newline='') as fout:
            writer = csv.DictWriter(
                fout,
                fieldnames=[
                    'shortDTE', 'longDTE', 'shortDelta', 'longDelta',
                    'rollDTE', 'gammaScalp', 'gammaDeltaThresh',
                    'profitTake', 'lossCut', 'units',
                    'annualReturn', 'maxDrawdown', 'score',
                    'totalOptGain', 'totalStkGain', 'finalNLV',
                    'numRolls', 'numGammaHedges',
                ])
            writer.writeheader()
            for param, result in ranked_rules:
                writer.writerow({
                    'shortDTE': param.short_dte,
                    'longDTE': param.long_dte,
                    'shortDelta': param.short_delta,
                    'longDelta': param.long_delta,
                    'rollDTE': param.roll_dte,
                    'gammaScalp': int(param.enable_gamma_scalp),
                    'gammaDeltaThresh': param.gamma_scalp_delta_thresh,
                    'profitTake': int(param.enable_profit_take),
                    'lossCut': int(param.enable_loss_cut),
                    'units': param.units,
                    'annualReturn': round(result['annualReturn'], 4),
                    'maxDrawdown': round(result['maxDrawdown'], 4),
                    'score': round(result['score'], 4),
                    'totalOptGain': round(
                        result['stat'].total_put_capital_gain
                        + result['stat'].total_call_capital_gain, 2),
                    'totalStkGain': round(result['stat'].total_stock_capital_gain, 2),
                    'finalNLV': round(result['finalNLV'], 2),
                    'numRolls': result['numRolls'],
                    'numGammaHedges': result['numGammaHedges'],
                })
        print(f'RULES done in {time.time()-t0:.0f}s', file=sys.stderr)
        print('\n=== RULES top 10 ===', file=sys.stderr)
        for param, result in ranked_rules[:10]:
            print(f'  score={result["score"]:.2f} ret={result["annualReturn"]:.2f}% '
                  f'dd={result["maxDrawdown"]:.2f}% | {param.label()} '
                  f'pt={int(param.enable_profit_take)} lc={int(param.enable_loss_cut)}',
                  file=sys.stderr)
    else:
        ranked_rules = None

    if args.stage in ('all', 'holdout'):
        # pick best overall
        best_param = None
        if ranked_rules:
            best_param = ranked_rules[0][0]
        elif ranked_refine:
            best_param = ranked_refine[0][0]
        elif ranked_coarse:
            best_param = ranked_coarse[0][0]
        else:
            raise SystemExit('No best param for holdout')

        df = ensure_cache(args.input_path, '2020-01-01', '2021-12-31', 1, holdout_cache)
        print(f'\n=== HOLDOUT 2020-2021 best: {best_param.label()} ===', file=sys.stderr)
        best_param.fixed_param = fixed
        result = w_strategy(df, best_param, debug=False, PositionsAndStats=PositionsAndStats)
        result['score'] = score_result(result)
        with open('spx-search-holdout.csv', 'w', encoding='UTF8', newline='') as fout:
            writer = csv.DictWriter(
                fout,
                fieldnames=[
                    'shortDTE', 'longDTE', 'shortDelta', 'longDelta',
                    'rollDTE', 'gammaScalp', 'gammaDeltaThresh',
                    'profitTake', 'lossCut', 'units',
                    'annualReturn', 'maxDrawdown', 'score',
                    'totalOptGain', 'totalStkGain', 'finalNLV',
                    'numRolls', 'numGammaHedges',
                ])
            writer.writeheader()
            writer.writerow({
                'shortDTE': best_param.short_dte,
                'longDTE': best_param.long_dte,
                'shortDelta': best_param.short_delta,
                'longDelta': best_param.long_delta,
                'rollDTE': best_param.roll_dte,
                'gammaScalp': int(best_param.enable_gamma_scalp),
                'gammaDeltaThresh': best_param.gamma_scalp_delta_thresh,
                'profitTake': int(best_param.enable_profit_take),
                'lossCut': int(best_param.enable_loss_cut),
                'units': best_param.units,
                'annualReturn': round(result['annualReturn'], 4),
                'maxDrawdown': round(result['maxDrawdown'], 4),
                'score': round(result['score'], 4),
                'totalOptGain': round(
                    result['stat'].total_put_capital_gain
                    + result['stat'].total_call_capital_gain, 2),
                'totalStkGain': round(result['stat'].total_stock_capital_gain, 2),
                'finalNLV': round(result['finalNLV'], 2),
                'numRolls': result['numRolls'],
                'numGammaHedges': result['numGammaHedges'],
            })
        print(f'HOLDOUT ret={result["annualReturn"]:.2f}% dd={result["maxDrawdown"]:.2f}% '
              f'score={result["score"]:.2f}', file=sys.stderr)


if __name__ == '__main__':
    main()
