#!/usr/bin/env python3
"""Focused refine around the best coarse SPX W parameters."""

from __future__ import annotations

import csv
import os
import sys
import time
import warnings

from Param import FixedParam, WParam
from Positions import PositionsAndStats
from StrategyW import w_strategy
from Util import round_float
from PriceDataUtil import Monthly_fields, load_yield_data
from WBacktest import _NullDictWriter, resolve_yield_path
from run_spx_search import CACHE_DIR, ensure_cache, score_result


def run_one(df, fixed, param):
    param.fixed_param = fixed
    t0 = time.time()
    result = w_strategy(df, param, debug=False, PositionsAndStats=PositionsAndStats)
    result['score'] = score_result(result)
    result['elapsed'] = time.time() - t0
    return result


def main():
    warnings.filterwarnings('ignore', category=RuntimeWarning, module=r'optlib\.gbs')
    load_yield_data(resolve_yield_path('data/yield-curve-rates-1990-2024.csv'))
    fixed = FixedParam(
        initial_balance=1_000_000, symbol='SPX',
        monthly_writer=_NullDictWriter(Monthly_fields),
        summary_writer=_NullDictWriter(),
    )

    # Best coarse: 45/90, Δ 0.45/0.12
    base = dict(
        short_dte=45, long_dte=90, short_delta=0.45, long_delta=0.12,
        n_call=1, n_put=1, units=10,
        profit_take_pct=50, loss_cut_pct=100,
        eval_expected_pnl=False,
    )

    cache = os.path.join(CACHE_DIR, 'spx_2018_2019_e5_atm.pkl')
    df = ensure_cache('../options-data/SPX.csv', '2018-01-01', '2019-12-31', 5, cache)

    configs = []
    # Wing ratio and gamma/roll combinations around the winner
    for m in (2, 3):
        for gs in (False, True):
            for rd in (0, 7):
                for gth in ((0.15,) if not gs else (0.10, 0.20)):
                    for pt in (True, False):
                        configs.append(WParam(
                            fixed_param=fixed,
                            m_call=m, m_put=m,
                            enable_gamma_scalp=gs,
                            roll_dte=rd,
                            gamma_scalp_delta_thresh=gth,
                            enable_profit_take=pt,
                            enable_loss_cut=True,
                            enable_dte_roll=True,
                            **base,
                        ))

    print(f'Refine configs: {len(configs)}', file=sys.stderr)
    results = []
    for i, param in enumerate(configs):
        print(f'[{i+1}/{len(configs)}] {param.label()} '
              f'M={param.m_call} pt={int(param.enable_profit_take)}',
              file=sys.stderr)
        result = run_one(df, fixed, param)
        results.append((param, result))
        print(f'  score={result["score"]:.2f} ret={result["annualReturn"]:.2f}% '
              f'dd={result["maxDrawdown"]:.2f}% rolls={result["numRolls"]} '
              f'hedges={result["numGammaHedges"]} ({result["elapsed"]:.0f}s)',
              file=sys.stderr)

    ranked = sorted(results, key=lambda x: -x[1]['score'])
    out = 'spx-search-refine.csv'
    with open(out, 'w', encoding='UTF8', newline='') as f:
        w = csv.DictWriter(f, fieldnames=[
            'shortDTE', 'longDTE', 'shortDelta', 'longDelta', 'm',
            'rollDTE', 'gammaScalp', 'gammaDeltaThresh', 'profitTake',
            'annualReturn', 'maxDrawdown', 'score',
            'totalOptGain', 'totalStkGain', 'finalNLV',
            'numRolls', 'numGammaHedges',
        ])
        w.writeheader()
        for param, result in ranked:
            w.writerow({
                'shortDTE': param.short_dte,
                'longDTE': param.long_dte,
                'shortDelta': param.short_delta,
                'longDelta': param.long_delta,
                'm': param.m_call,
                'rollDTE': param.roll_dte,
                'gammaScalp': int(param.enable_gamma_scalp),
                'gammaDeltaThresh': param.gamma_scalp_delta_thresh,
                'profitTake': int(param.enable_profit_take),
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

    print(f'\nWrote {out}', file=sys.stderr)
    print('=== REFINE top 15 ===', file=sys.stderr)
    for param, result in ranked[:15]:
        print(f'  score={result["score"]:.2f} ret={result["annualReturn"]:.2f}% '
              f'dd={result["maxDrawdown"]:.2f}% | {param.label()} '
              f'M={param.m_call} pt={int(param.enable_profit_take)}',
              file=sys.stderr)

    # Holdout best on 2020-2021
    best_param, best_res = ranked[0]
    holdout = os.path.join(CACHE_DIR, 'spx_2020_2021_e5_atm.pkl')
    df_h = ensure_cache('../options-data/SPX.csv', '2020-01-01', '2021-12-31', 5, holdout)
    print(f'\n=== HOLDOUT best {best_param.label()} M={best_param.m_call} ===',
          file=sys.stderr)
    h = run_one(df_h, fixed, best_param)
    with open('spx-search-holdout.csv', 'w', encoding='UTF8', newline='') as f:
        w = csv.DictWriter(f, fieldnames=[
            'window', 'shortDTE', 'longDTE', 'shortDelta', 'longDelta', 'm',
            'rollDTE', 'gammaScalp', 'gammaDeltaThresh', 'profitTake',
            'annualReturn', 'maxDrawdown', 'score',
            'totalOptGain', 'totalStkGain', 'finalNLV',
            'numRolls', 'numGammaHedges',
        ])
        w.writeheader()
        for label, param, result in (
            ('train_2018_2019', best_param, best_res),
            ('holdout_2020_2021', best_param, h),
        ):
            w.writerow({
                'window': label,
                'shortDTE': param.short_dte,
                'longDTE': param.long_dte,
                'shortDelta': param.short_delta,
                'longDelta': param.long_delta,
                'm': param.m_call,
                'rollDTE': param.roll_dte,
                'gammaScalp': int(param.enable_gamma_scalp),
                'gammaDeltaThresh': param.gamma_scalp_delta_thresh,
                'profitTake': int(param.enable_profit_take),
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
    print(f'HOLDOUT ret={h["annualReturn"]:.2f}% dd={h["maxDrawdown"]:.2f}% '
          f'score={h["score"]:.2f}', file=sys.stderr)


if __name__ == '__main__':
    main()
