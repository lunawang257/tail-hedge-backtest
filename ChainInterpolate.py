#!/usr/bin/env python3
"""Interpolate option chains for non-listed (odd) days-to-expiration.

When the strategy requests e.g. 9 DTE but only 7 and 14 DTE expiries exist,
we interpolate IV (and optionally skew across strikes) between the bracketing
chains, use the tenor-matched risk-free rate, and price via Merton/BS.

Assumes European-style options (no early exercise / assignment).
"""

from __future__ import annotations

import math
import sys

import pandas as pd

from PriceDataUtil import (
    get_risk_free_rate,
    get_dividend_yield,
    sanitize_option_price_for_iv,
    euro_implied_vol,
    merton,
)


def available_expirations(group, as_of=None):
    """Sorted unique expiration dates on or after as_of (today)."""
    exps = group['ExpirationDate']
    if hasattr(exps, 'dt'):
        vals = exps.drop_duplicates().tolist()
    else:
        vals = list(exps.unique())
    exps = sorted(pd.Timestamp(e).normalize() for e in vals)
    if as_of is None:
        return exps
    as_of = pd.Timestamp(as_of).normalize()
    return [e for e in exps if e >= as_of]


def nearest_listed_expiration(group, as_of, target_dte):
    """Pick the listed expiry whose DTE is closest to target_dte."""
    as_of = pd.Timestamp(as_of).normalize()
    exps = available_expirations(group, as_of)
    if not exps:
        return None
    target = as_of + pd.Timedelta(days=int(target_dte))
    return min(exps, key=lambda e: abs((e - target).days))


def bracketing_expirations(group, as_of, target_dte):
    """Return (lo_exp, hi_exp) that bracket target_dte, or (nearest, None)."""
    as_of = pd.Timestamp(as_of).normalize()
    exps = available_expirations(group, as_of)
    if not exps:
        return None, None
    dtes = [(e, (e - as_of).days) for e in exps]
    below = [x for x in dtes if x[1] <= target_dte]
    above = [x for x in dtes if x[1] >= target_dte]
    if below and above:
        lo = max(below, key=lambda x: x[1])[0]
        hi = min(above, key=lambda x: x[1])[0]
        return lo, hi
    nearest = min(dtes, key=lambda x: abs(x[1] - target_dte))[0]
    return nearest, None


def _mid_row(row):
    mid = (float(row.BidPrice) + float(row.AskPrice)) / 2.0
    if mid > 0 and math.isfinite(mid):
        return mid
    last = float(getattr(row, 'LastPrice', 0) or 0)
    if last > 0 and math.isfinite(last):
        return last
    return 0.0


def _ensure_quote_cols(group):
    """Ensure Bid/Ask are usable; fill zeros from LastPrice when present."""
    if 'LastPrice' not in group.columns:
        return group
    out = group
    mid = (out['BidPrice'] + out['AskPrice']) / 2.0
    bad = (mid <= 0) | mid.isna()
    if bad.any():
        out = out.copy()
        fill = out.loc[bad, 'LastPrice'].clip(lower=0)
        out.loc[bad, 'BidPrice'] = fill
        out.loc[bad, 'AskPrice'] = fill
    return out


def chain_iv_by_moneyness(group, opt_type, exp_date, stock_price, as_of):
    """Map strike/spot moneyness -> IV for one expiry/type."""
    exp = pd.Timestamp(exp_date).normalize()
    as_of = pd.Timestamp(as_of).normalize()
    days = max((exp - as_of).days, 1)
    t = days / 365.25
    r = get_risk_free_rate(as_of, t)
    q = get_dividend_yield(as_of)
    rows = group[
        (group['PutCall'] == opt_type) &
        (group['ExpirationDate'].dt.normalize() == exp)
    ]
    points = []
    for row in rows.itertuples(index=False):
        mid = _mid_row(row)
        if (not math.isfinite(mid)) or mid <= 0:
            continue
        k = float(row.StrikePrice)
        try:
            iv_price = sanitize_option_price_for_iv(
                opt_type, stock_price, k, t, r, q, mid)
            if iv_price is None:
                continue
            iv = euro_implied_vol(
                opt_type, fs=stock_price, x=k, t=t, r=r, q=q, cp=iv_price)
            if (not math.isfinite(iv)) or iv <= 0:
                continue
            points.append((k / stock_price, iv, k, mid))
        except Exception:
            continue
    points.sort(key=lambda p: p[0])
    return points


def _interp_iv_at_moneyness(points, moneyness):
    """Linear interpolate IV in moneyness space."""
    if not points:
        return None
    if moneyness <= points[0][0]:
        return points[0][1]
    if moneyness >= points[-1][0]:
        return points[-1][1]
    for i in range(1, len(points)):
        m0, iv0 = points[i - 1][0], points[i - 1][1]
        m1, iv1 = points[i][0], points[i][1]
        if m0 <= moneyness <= m1:
            if m1 == m0:
                return iv0
            w = (moneyness - m0) / (m1 - m0)
            return iv0 * (1 - w) + iv1 * w
    return points[-1][1]


def _anchor_iv(group, opt_type, exp_date, stock_price, as_of):
    """ATM-ish IV from the nearest strikes (fast, used for delta shortlist)."""
    exp = pd.Timestamp(exp_date).normalize()
    as_of = pd.Timestamp(as_of).normalize()
    days = max((exp - as_of).days, 1)
    t = days / 365.25
    r = get_risk_free_rate(as_of, t)
    q = get_dividend_yield(as_of)
    rows = group[
        (group['PutCall'] == opt_type) &
        (group['ExpirationDate'].dt.normalize() == exp)
    ]
    ranked = sorted(
        ((abs(float(row.StrikePrice) - stock_price), row) for row in rows.itertuples(index=False)),
        key=lambda x: x[0],
    )
    for _, row in ranked[:16]:
        mid = _mid_row(row)
        if mid <= 0:
            continue
        k = float(row.StrikePrice)
        try:
            iv_price = sanitize_option_price_for_iv(
                opt_type, stock_price, k, t, r, q, mid)
            if iv_price is None:
                continue
            iv = euro_implied_vol(
                opt_type, fs=stock_price, x=k, t=t, r=r, q=q, cp=iv_price)
            if math.isfinite(iv) and iv > 0:
                return iv
        except Exception:
            continue
    return 0.20


def interpolate_iv_for_dte(group, opt_type, as_of, stock_price, strike, target_dte,
                           iv_cache=None):
    """Interpolate IV at ``strike`` for a synthetic ``target_dte`` expiry."""
    as_of = pd.Timestamp(as_of).normalize()
    lo, hi = bracketing_expirations(group, as_of, target_dte)
    if lo is None:
        return None, None

    moneyness = float(strike) / float(stock_price)
    t_target = max(target_dte, 1) / 365.25

    def pts(exp):
        key = (opt_type, pd.Timestamp(exp).value)
        if iv_cache is not None and key in iv_cache:
            return iv_cache[key]
        p = chain_iv_by_moneyness(group, opt_type, exp, stock_price, as_of)
        if iv_cache is not None:
            iv_cache[key] = p
        return p

    lo_pts = pts(lo)
    iv_lo = _interp_iv_at_moneyness(lo_pts, moneyness)
    if iv_lo is None:
        return None, None

    if hi is None or hi == lo:
        return iv_lo, lo

    hi_pts = pts(hi)
    iv_hi = _interp_iv_at_moneyness(hi_pts, moneyness)
    if iv_hi is None:
        return iv_lo, lo

    t_lo = max((lo - as_of).days, 1) / 365.25
    t_hi = max((hi - as_of).days, 1) / 365.25
    var_lo = iv_lo * iv_lo * t_lo
    var_hi = iv_hi * iv_hi * t_hi
    if t_hi == t_lo:
        iv = iv_lo
    else:
        w = (t_target - t_lo) / (t_hi - t_lo)
        var = var_lo * (1 - w) + var_hi * w
        iv = math.sqrt(max(var / t_target, 1e-12))
    return iv, (lo, hi)


def synthetic_option_quote(group, opt_type, as_of, stock_price, strike, target_dte,
                           slip_pct=0.0, iv_cache=None):
    """Build a synthetic bid/ask mid quote for an odd-DTE option."""
    as_of = pd.Timestamp(as_of).normalize()
    target_exp = as_of + pd.Timedelta(days=int(target_dte))
    t = max(target_dte, 1) / 365.25
    r = get_risk_free_rate(as_of, t)
    q = get_dividend_yield(as_of)

    # Prefer exact listed quote when an expiry matches target_dte exactly.
    listed = group[
        (group['PutCall'] == opt_type) &
        (group['ExpirationDate'].dt.normalize() == target_exp) &
        ((group['StrikePrice'] - strike).abs() < 1e-6)
    ]
    if not listed.empty:
        row = listed.iloc[0]
        mid = (float(row['BidPrice']) + float(row['AskPrice'])) / 2.0
        try:
            iv_price = sanitize_option_price_for_iv(
                opt_type, stock_price, strike, t, r, q, mid)
            iv = euro_implied_vol(
                opt_type, fs=stock_price, x=strike, t=t, r=r, q=q, cp=iv_price)
            value, delta, gamma, theta, vega, rho = merton(
                opt_type, fs=stock_price, x=strike, t=t, r=r, q=q, v=iv)
        except Exception:
            iv, delta, gamma, theta = 0.0, 0.0, 0.0, 0.0
            value = mid
        return {
            'BidPrice': float(row['BidPrice']),
            'AskPrice': float(row['AskPrice']),
            'StrikePrice': float(strike),
            'ExpirationDate': target_exp,
            'DataDate': as_of,
            'UnderlyingPrice': float(stock_price),
            'PutCall': opt_type,
            'implied_vol': float(iv) if math.isfinite(iv) else 0.0,
            'delta': float(delta) if math.isfinite(delta) else 0.0,
            'gamma': float(gamma) if math.isfinite(gamma) else 0.0,
            'theta': float(theta) if math.isfinite(theta) else 0.0,
            'interpolated': False,
            'mid': mid,
        }

    iv, sources = interpolate_iv_for_dte(
        group, opt_type, as_of, stock_price, strike, target_dte, iv_cache=iv_cache)
    if iv is None:
        return None

    value, delta, gamma, theta, vega, rho = merton(
        opt_type, fs=stock_price, x=strike, t=t, r=r, q=q, v=iv)
    if not math.isfinite(value):
        return None

    mid = float(value)
    half = mid * slip_pct
    return {
        'BidPrice': max(mid - half, 0.0),
        'AskPrice': mid + half,
        'StrikePrice': float(strike),
        'ExpirationDate': target_exp,
        'DataDate': as_of,
        'UnderlyingPrice': float(stock_price),
        'PutCall': opt_type,
        'implied_vol': float(iv),
        'delta': float(delta),
        'gamma': float(gamma),
        'theta': float(theta),
        'interpolated': True,
        'iv_sources': sources,
        'mid': mid,
    }


_DELTA_SHORTLIST = 12


def find_option_by_delta(group, opt_type, as_of, stock_price, target_abs_delta,
                         target_dte, prefer_otm=True, slip_pct=0.01,
                         allow_interpolate=True):
    """Find the strike at ~target_dte whose |delta| is closest to target_abs_delta.

    Fast path: rank strikes with a single ATM IV (Merton deltas), then fully
    price / interpolate only a shortlist of candidates.
    """
    as_of = pd.Timestamp(as_of).normalize()
    target_sign = -1.0 if opt_type == 'p' else 1.0
    target_delta = target_sign * abs(target_abs_delta)

    group = _ensure_quote_cols(group)
    lo, hi = bracketing_expirations(group, as_of, target_dte)
    if lo is None:
        return None

    # Use nearest listed expiry for candidate strikes + anchor IV
    nearest = lo if hi is None or abs((lo - as_of).days - target_dte) <= abs((hi - as_of).days - target_dte) else hi
    listed_dte = max((nearest - as_of).days, 1)
    use_dte = target_dte if allow_interpolate else listed_dte

    # Bound strike search around spot — critical for dense SPX chains
    # Wide wings for small deltas, tighter for near-ATM shorts.
    width = 0.08 + 0.55 * (1.0 - abs(target_abs_delta))
    lo_k = stock_price * (1.0 - width)
    hi_k = stock_price * (1.0 + width)

    candidates = group[
        (group['PutCall'] == opt_type) &
        (group['ExpirationDate'].dt.normalize() == nearest) &
        (group['StrikePrice'] >= lo_k) &
        (group['StrikePrice'] <= hi_k)
    ]
    if candidates.empty:
        candidates = group[
            (group['PutCall'] == opt_type) &
            (group['ExpirationDate'].dt.normalize() == nearest)
        ]
    if candidates.empty:
        return None

    t = max(use_dte, 1) / 365.25
    r = get_risk_free_rate(as_of, t)
    q = get_dividend_yield(as_of)
    anchor = _anchor_iv(group, opt_type, nearest, stock_price, as_of)

    ranked = []
    for row in candidates.itertuples(index=False):
        mid = _mid_row(row)
        if mid <= 0:
            continue
        k = float(row.StrikePrice)
        if prefer_otm:
            if opt_type == 'c' and k < stock_price * 0.97:
                continue
            if opt_type == 'p' and k > stock_price * 1.03:
                continue
        try:
            _, delta, *_rest = merton(
                opt_type, fs=stock_price, x=k, t=t, r=r, q=q, v=anchor)
            if not math.isfinite(delta):
                continue
        except Exception:
            continue
        ranked.append((abs(delta - target_delta), k, mid, row))

    if not ranked:
        return None
    ranked.sort(key=lambda x: x[0])

    iv_cache = {}
    best = None
    best_err = None
    for err_approx, k, mid, row in ranked[:_DELTA_SHORTLIST]:
        # Prefer listed nearest expiry pricing unless target DTE differs a lot
        price_dte = use_dte if allow_interpolate else listed_dte
        if allow_interpolate and abs(listed_dte - target_dte) >= 2:
            quote = synthetic_option_quote(
                group, opt_type, as_of, stock_price, k, target_dte,
                slip_pct=slip_pct, iv_cache=iv_cache)
        else:
            quote = synthetic_option_quote(
                group, opt_type, as_of, stock_price, k, listed_dte,
                slip_pct=slip_pct, iv_cache=iv_cache)
            if quote is None:
                try:
                    value, delta, gamma, theta, vega, rho = merton(
                        opt_type, fs=stock_price, x=k, t=max(listed_dte, 1) / 365.25,
                        r=r, q=q, v=anchor)
                except Exception:
                    continue
                half = float(value) * slip_pct
                quote = {
                    'BidPrice': max(float(value) - half, 0.0),
                    'AskPrice': float(value) + half,
                    'StrikePrice': k,
                    'ExpirationDate': nearest,
                    'DataDate': as_of,
                    'UnderlyingPrice': float(stock_price),
                    'PutCall': opt_type,
                    'implied_vol': anchor,
                    'delta': float(delta),
                    'gamma': float(gamma),
                    'theta': float(theta),
                    'interpolated': abs(listed_dte - target_dte) >= 2,
                    'mid': float(value),
                }
        if quote is None:
            continue
        err = abs(quote['delta'] - target_delta)
        if best_err is None or err < best_err:
            best_err = err
            best = quote

    return best
