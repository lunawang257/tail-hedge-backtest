#!/usr/bin/env python3
"""Dump Python reference numbers for C++ parity tests + timing."""
from __future__ import annotations
import json, time, sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..'))

from PriceDataUtil import load_yield_data, merton, euro_implied_vol
from Param import FixedParam, WParam
from Positions import PositionsAndStats
from StrategyW import w_strategy
from WBacktest import load_options_csv, _NullDictWriter
from PriceDataUtil import Monthly_fields
from ChainInterpolate import find_option_by_delta
import pandas as pd

ROOT = os.path.join(os.path.dirname(__file__), '..', '..')
YIELD = os.path.join(ROOT, 'data', 'yield-curve-rates-1990-2024.csv')
FIXTURE = os.path.join(ROOT, 'data', 'SPY_fixture_small.csv')
OUT = os.path.join(os.path.dirname(__file__), 'python_reference.json')


def main():
    load_yield_data(YIELD)

    # GBS spot checks
    gbs_cases = []
    for typ, fs, x, t, r, q, v in [
        ('c', 100, 100, 0.5, 0.05, 0.02, 0.20),
        ('p', 100, 100, 0.5, 0.05, 0.02, 0.20),
        ('c', 120, 100, 30/365.25, 0.04, 0.015, 0.25),
        ('p', 90, 100, 45/365.25, 0.03, 0.01, 0.30),
    ]:
        val, delta, gamma, theta, vega, rho = merton(typ, fs, x, t, r, q, v)
        # round-trip IV
        iv = euro_implied_vol(typ, fs, x, t, r, q, val)
        gbs_cases.append({
            'type': typ, 'fs': fs, 'x': x, 't': t, 'r': r, 'q': q, 'v': v,
            'value': val, 'delta': delta, 'gamma': gamma, 'theta': theta,
            'vega': vega, 'rho': rho, 'iv_roundtrip': iv,
        })

    fixed = FixedParam(initial_balance=100_000, symbol='SPY')
    fixed.summary_writer = _NullDictWriter()
    fixed.monthly_writer = _NullDictWriter(Monthly_fields)

    df = load_options_csv(FIXTURE)
    # Delta pick on first liquid day
    day = sorted(df.DataDate.unique())[0]
    group = df[df.DataDate == day]
    spot = float(group.UnderlyingPrice.iloc[0])
    quotes = {}
    for name, ot, dte, abs_d in [
        ('short_call', 'c', 30, 0.40),
        ('short_put', 'p', 30, 0.40),
        ('long_call', 'c', 60, 0.10),
        ('long_put', 'p', 60, 0.10),
    ]:
        q = find_option_by_delta(group, ot, day, spot, abs_d, dte)
        quotes[name] = {
            'strike': float(q['StrikePrice']),
            'mid': float(q['mid']),
            'delta': float(q['delta']),
            'iv': float(q['implied_vol']),
            'exp': pd.Timestamp(q['ExpirationDate']).strftime('%Y-%m-%d'),
        }

    param = WParam(
        fixed_param=fixed,
        short_dte=30, long_dte=60,
        short_delta=0.40, long_delta=0.10,
        n_call=1, n_put=1, m_call=2, m_put=2,
        units=1, roll_dte=0,
        enable_gamma_scalp=False,
        enable_profit_take=True,
        enable_loss_cut=True,
        eval_expected_pnl=False,
    )

    t0 = time.perf_counter()
    n_bench = 5
    result = None
    for _ in range(n_bench):
        result = w_strategy(df, param, debug=False, PositionsAndStats=PositionsAndStats)
    elapsed_ms = (time.perf_counter() - t0) / n_bench * 1000

    ref = {
        'gbs': gbs_cases,
        'delta_picks': quotes,
        'spot': spot,
        'as_of': pd.Timestamp(day).strftime('%Y-%m-%d'),
        'backtest': {
            'annualReturn': float(result['annualReturn']),
            'maxDrawdown': float(result['maxDrawdown']),
            'finalNLV': float(result['finalNLV']),
            'numRolls': int(result['numRolls']),
            'elapsed_ms_python': elapsed_ms,
        },
        'param': {
            'short_dte': 30, 'long_dte': 60,
            'short_delta': 0.40, 'long_delta': 0.10,
            'm_call': 2, 'm_put': 2, 'units': 1, 'roll_dte': 0,
            'balance': 100000,
        },
    }
    with open(OUT, 'w') as f:
        json.dump(ref, f, indent=2)
    print(f'Wrote {OUT}')
    print(f"Python backtest annual={result['annualReturn']:.4f}% "
          f"dd={result['maxDrawdown']:.4f}% finalNLV={result['finalNLV']:.2f} "
          f"rolls={result['numRolls']}  {elapsed_ms:.1f} ms/run")


if __name__ == '__main__':
    main()
