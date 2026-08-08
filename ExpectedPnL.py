#!/usr/bin/env python3
"""Expected P/L: sum over price moves of (PnL(move) * P(move)).

Probabilities default to a lognormal / Black-Scholes density using recent
realized vol (or ATM IV when available). Also supports empirical histogram
probabilities from the input price history.
"""

from __future__ import annotations

import math

import numpy as np
import pandas as pd

from OptionPos import CONTRACT_SCALE
from PriceDataUtil import get_risk_free_rate, get_dividend_yield, merton


def realized_vol(spots, lookback=20):
    """Annualized realized vol from consecutive spot samples."""
    if len(spots) < 3:
        return 0.20
    arr = np.asarray(spots[-lookback:], dtype=float)
    rets = np.diff(np.log(arr))
    if len(rets) == 0:
        return 0.20
    return float(np.std(rets, ddof=1) * math.sqrt(252))


def lognormal_probs(spot, move_pcts, vol, days_horizon=5, r=0.0):
    """Probability mass for discrete return buckets under lognormal dynamics.

    Each move_pct is treated as the center of a bucket whose edges are midpoints
    to neighboring moves; outermost buckets are open-ended.
    """
    moves = sorted(float(m) for m in move_pcts)
    t = max(days_horizon, 1) / 365.25
    if vol <= 0 or t <= 0:
        # Degenerate: all mass on 0 move
        return {m: (1.0 if abs(m) < 1e-12 else 0.0) for m in moves}

    mu = (r - 0.5 * vol * vol) * t
    sig = vol * math.sqrt(t)

    # Bucket edges in return space
    edges = [-float('inf')]
    for i in range(len(moves) - 1):
        edges.append(0.5 * (moves[i] + moves[i + 1]))
    edges.append(float('inf'))

    from scipy.stats import norm
    probs = {}
    for i, m in enumerate(moves):
        # P(log(1+R) in [lo, hi]) where R ~ move
        lo = edges[i]
        hi = edges[i + 1]
        # Convert return edges to log-price change: log(S'/S) = log(1+R)
        def to_z(ret):
            if ret <= -0.999999:
                return -float('inf')
            return (math.log(1.0 + ret) - mu) / sig

        z_lo = to_z(lo) if math.isfinite(lo) else -float('inf')
        z_hi = to_z(hi) if math.isfinite(hi) else float('inf')
        probs[m] = float(norm.cdf(z_hi) - norm.cdf(z_lo))

    total = sum(probs.values())
    if total <= 0:
        return {m: 1.0 / len(moves) for m in moves}
    return {m: p / total for m, p in probs.items()}


def empirical_probs(spots, move_pcts, horizon_days=5):
    """Histogram probabilities from historical multi-day returns."""
    moves = sorted(float(m) for m in move_pcts)
    if len(spots) <= horizon_days + 1:
        return {m: 1.0 / len(moves) for m in moves}

    arr = np.asarray(spots, dtype=float)
    rets = arr[horizon_days:] / arr[:-horizon_days] - 1.0
    edges = [-float('inf')]
    for i in range(len(moves) - 1):
        edges.append(0.5 * (moves[i] + moves[i + 1]))
    edges.append(float('inf'))

    counts = {m: 0 for m in moves}
    for r in rets:
        for i, m in enumerate(moves):
            if edges[i] <= r < edges[i + 1]:
                counts[m] += 1
                break
    total = sum(counts.values()) or 1
    return {m: counts[m] / total for m in moves}


def structure_pnl_at_spot(opt_list, new_spot, as_of, entry_cash, r=None, q=None,
                          days_to_horizon=0):
    """Mark option structure at a hypothetical spot (IV held constant).

    Advances calendar by ``days_to_horizon`` for theta decay. Returns P/L vs
    entry_cash (same convention as PositionsAndStats.structure_pnl).
    """
    as_of = pd.Timestamp(as_of).normalize()
    mark_date = as_of + pd.Timedelta(days=int(days_to_horizon))
    opt_nlv = 0.0
    for opt in opt_list:
        days = (pd.Timestamp(opt.exp_date) - mark_date).days
        if days <= 0:
            if opt.opt_type == 'p':
                mid = max(0.0, float(opt.strike) - float(new_spot))
            else:
                mid = max(0.0, float(new_spot) - float(opt.strike))
        else:
            t = days / 365.25
            rf = r if r is not None else get_risk_free_rate(as_of, t)
            div = q if q is not None else get_dividend_yield(as_of)
            iv = opt.implied_vol if opt.implied_vol > 0 else 0.20
            try:
                value, *_ = merton(
                    opt.opt_type, fs=float(new_spot), x=float(opt.strike),
                    t=t, r=rf, q=div, v=iv)
                mid = float(value) if math.isfinite(value) else 0.0
            except Exception:
                mid = 0.0
        opt_nlv += mid * opt.num_contracts * CONTRACT_SCALE
    return opt_nlv - entry_cash


def expected_structure_pnl(opt_list, spot, as_of, entry_cash, move_pcts,
                           spots_history=None, horizon_days=5,
                           use_empirical=False, vol=None):
    """E[PnL] = sum_i PnL(spot*(1+m_i)) * P(m_i)."""
    if not opt_list:
        return 0.0, {}, {}

    if vol is None:
        vol = realized_vol(spots_history or [spot], lookback=20)
    r = get_risk_free_rate(as_of, horizon_days / 365.25)

    if use_empirical and spots_history is not None:
        probs = empirical_probs(spots_history, move_pcts, horizon_days)
    else:
        probs = lognormal_probs(spot, move_pcts, vol, days_horizon=horizon_days, r=r)

    pnls = {}
    expected = 0.0
    for m, p in probs.items():
        pnl = structure_pnl_at_spot(
            opt_list, spot * (1.0 + m), as_of, entry_cash,
            r=r, days_to_horizon=horizon_days)
        pnls[m] = pnl
        expected += pnl * p
    return expected, pnls, probs
