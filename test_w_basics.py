#!/usr/bin/env python3
"""Lightweight unit checks for chain interpolation and expected P/L."""

import math
import pandas as pd

from PriceDataUtil import load_yield_data, adjust_prices_for_splits_and_merges
from ChainInterpolate import find_option_by_delta, bracketing_expirations
from ExpectedPnL import (
    lognormal_probs, expected_structure_pnl, expected_pnl_by_ranges,
)
from OptionPos import OptPosition
from Param import WParam, FixedParam, parse_float_range
from StrategyW import choose_wing_counts


def test_parse_range():
    assert parse_float_range('30', as_int=True) == [30]
    assert parse_float_range('21,35,7', as_int=True) == [21, 28, 35]
    assert abs(parse_float_range('0.35,0.45,0.05')[0] - 0.35) < 1e-9


def test_wparam_validation():
    fp = FixedParam()
    p = WParam(fixed_param=fp, short_dte=30, long_dte=60,
               short_delta=0.4, long_delta=0.1, n_call=1, m_call=2)
    assert 'sDTE=30' in p.label()
    try:
        WParam(fixed_param=fp, short_delta=0.1, long_delta=0.4)
        assert False, 'expected ValueError'
    except ValueError:
        pass
    c = WParam(fixed_param=fp, wing_mode='credit', n_call=10, n_put=10,
               m_call=10, m_put=10, max_wing_mult=3)
    assert c.wing_mode == 'credit'
    t = WParam(fixed_param=fp, wing_mode='theta', theta_threshold=0.0)
    assert 'theta' in t.label()


def test_choose_wing_credit_and_theta():
    fp = FixedParam()
    short_c = {'mid': 10.0, 'BidPrice': 9.9, 'AskPrice': 10.1, 'theta': -0.05}
    short_p = {'mid': 10.0, 'BidPrice': 9.9, 'AskPrice': 10.1, 'theta': -0.05}
    long_c = {'mid': 1.5, 'BidPrice': 1.4, 'AskPrice': 1.6, 'theta': -0.02}
    long_p = {'mid': 1.5, 'BidPrice': 1.4, 'AskPrice': 1.6, 'theta': -0.02}
    p = WParam(fixed_param=fp, wing_mode='credit', n_call=10, n_put=10,
               m_call=10, m_put=10, credit_buffer=0.02, max_wing_mult=5)
    mc, mp = choose_wing_counts(p, short_c, short_p, long_c, long_p)
    assert mc >= 10 and mp >= 10
    assert mc > 10

    p2 = WParam(fixed_param=fp, wing_mode='theta', n_call=10, n_put=10,
                m_call=10, m_put=10, theta_threshold=0.0, max_wing_mult=5)
    mc2, mp2 = choose_wing_counts(p2, short_c, short_p, long_c, long_p)
    assert mc2 >= 10


def test_find_by_delta_and_expected_pnl():
    load_yield_data('data/yield-curve-rates-1990-2024.csv')
    df = pd.read_csv('data/SPY_50000.csv')
    df['DataDate'] = pd.to_datetime(df['DataDate'])
    df['ExpirationDate'] = pd.to_datetime(df['ExpirationDate'])
    df['PutCall'] = df['PutCall'].str.lower().str[0]
    df = adjust_prices_for_splits_and_merges(df)

    day = None
    for d in sorted(df.DataDate.unique()):
        g = df[df.DataDate == d]
        if ((g.BidPrice + g.AskPrice) > 0).sum() > 100:
            day = d
            group = g
            break
    assert day is not None
    spot = float(group.UnderlyingPrice.iloc[0])
    lo, hi = bracketing_expirations(group, day, 30)
    assert lo is not None

    q = find_option_by_delta(group, 'c', day, spot, 0.40, 30)
    assert q is not None
    assert q['StrikePrice'] > 0
    assert abs(q['delta']) > 0

    # Weekly/daily synthetic tenor (odd DTE) should still resolve
    q_w = find_option_by_delta(group, 'c', day, spot, 0.40, 9)
    assert q_w is not None
    assert q_w['StrikePrice'] > 0

    legs = []
    for opt_type, abs_d, dte, n in [
        ('c', 0.4, 30, -1), ('p', 0.4, 30, -1),
        ('c', 0.1, 60, 2), ('p', 0.1, 60, 2),
    ]:
        qq = find_option_by_delta(group, opt_type, day, spot, abs_d, dte)
        assert qq is not None
        legs.append(OptPosition(
            opt_type=opt_type, strike=qq['StrikePrice'],
            exp_date=qq['ExpirationDate'], num_contracts=n,
            open_price=qq['mid'], org_price=qq['mid'],
            delta=qq['delta'], gamma=qq['gamma'], theta=qq['theta'],
            implied_vol=qq['implied_vol'],
        ))
    entry = sum(o.get_cost_basis() for o in legs)
    exp, pnls, probs = expected_structure_pnl(
        legs, spot, day, entry, [-0.1, 0.0, 0.1],
        spots_history=[spot] * 30, horizon_days=5)
    assert math.isfinite(exp)
    assert abs(sum(probs.values()) - 1.0) < 1e-6
    assert 0.0 in pnls

    exp2, rows, _, _ = expected_pnl_by_ranges(
        legs, spot, day, entry,
        ranges=[(-0.10, -0.04), (-0.04, 0.04), (0.04, 0.10)],
        spots_history=[spot] * 30, horizon_days=5)
    assert math.isfinite(exp2)
    assert len(rows) == 3


def test_lognormal_probs_sum():
    probs = lognormal_probs(100, [-0.2, -0.1, 0, 0.1, 0.2], vol=0.2, days_horizon=5)
    assert abs(sum(probs.values()) - 1.0) < 1e-6


if __name__ == '__main__':
    test_parse_range()
    test_wparam_validation()
    test_choose_wing_credit_and_theta()
    test_lognormal_probs_sum()
    test_find_by_delta_and_expected_pnl()
    print('All tests passed.')
