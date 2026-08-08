#!/usr/bin/env python3
"""Utility functions for handling CSV data and price/option calculations."""

import math
import os
import sys

import pandas as pd

# Prefer the bundled optlib package over any globally injected checkout.
_LOCAL_OPTLIB_PATH = os.path.join(os.path.dirname(__file__), 'optlib')
if _LOCAL_OPTLIB_PATH not in sys.path:
    sys.path.insert(0, _LOCAL_OPTLIB_PATH)

from optlib.gbs import (
    merton,
    euro_implied_vol
)

from Util import round_float

# ============================================================================
# CSV Field Definitions
# ============================================================================

Summary_fields = [
    'shortDTE',
    'longDTE',
    'shortDelta',
    'longDelta',
    'nCall',
    'nPut',
    'mCall',
    'mPut',
    'units',
    'rollDTE',
    'gammaScalp',
    'gammaDeltaThresh',
    'profitTake',
    'lossCut',
    'annualReturn',
    'maxDrawdown',
    'taxRate',          # tax / finalNLV
    'commRate',         # commission / finalNLV
    'totalOptGain',
    'totalStkGain',
    'expectedPnL',      # last computed E[PnL] from move*probability
    'numRolls',
    'numGammaHedges',
]

Monthly_fields = [
    'date',
    'shortDTE',
    'longDTE',
    'shortDelta',
    'longDelta',
    'action',           # buy, sell, hedge, roll, mark
    'entType',          # stock, put, call, structure
    'legRole',          # short_call, short_put, long_call, long_put, hedge
    'reason',           # dte_roll, profit_take, loss_cut, gamma_scalp, open, expire

    'stkShares',        # negative means sell
    'stkOpenPrice',
    'stkPrice',

    'contracts',        # negative means short
    'optOpenDate',
    'optOpenPrice',
    'optPrice',
    'strike',
    'expDate',
    'interpolated',     # 1 if odd-DTE synthetic quote

    'stkGain',
    'optGain',
    'structurePnL',
    'structurePnLPct',
    'netDelta',
    'netGamma',
    'netTheta',
    'expectedPnL',

    'totalCalls',
    'totalPuts',
    'totalShares',
    'totalStkGain',
    'totalPutGain',
    'totalCallGain',
    'totalOptGain',
    'totalTax',
    'totalComm',
    'totalGain',
    'optNLV',
    'stockNLV',
    'cash',
    'TtlNLV',

    'maxDrawdown',
    'maxDrawDPeak',
    'maxDDPeakDate',
    'curDrawdown',
    'curPeak',
    'curPeakDate',
]


# ============================================================================
# Module-level state for yield and dividend data
# ============================================================================

YieldData = None
DivYieldData = None
YieldTenors = []
_logged_bs_failures = set()
_risk_free_rate_cache = {}
_dividend_yield_cache = {}

TREASURY_TENOR_YEARS = [
    ('1 Mo', 1 / 12),
    ('2 Mo', 2 / 12),
    ('3 Mo', 3 / 12),
    ('4 Mo', 4 / 12),
    ('6 Mo', 6 / 12),
    ('1 Yr', 1.0),
    ('2 Yr', 2.0),
    ('3 Yr', 3.0),
    ('5 Yr', 5.0),
    ('7 Yr', 7.0),
    ('10 Yr', 10.0),
    ('20 Yr', 20.0),
    ('30 Yr', 30.0),
]


# ============================================================================
# CSV Row Helper Functions
# ============================================================================

def add_val(row, key, val):
    """Add a value to a CSV row dictionary.
    
    Args:
        row: Dictionary representing a CSV row
        key: Key that must exist in row
        val: Value to add (floats are rounded)
    """
    assert key in row
    if isinstance(val, float):
        row[key] = round_float(val)
    else:
        row[key] = val


def init_csv_row(param, fields):
    """Initialize a CSV row with default values for W strategy params."""
    row = {key: 0 for key in fields}
    if param is None:
        return row
    mapping = {
        'shortDTE': getattr(param, 'short_dte', 0),
        'longDTE': getattr(param, 'long_dte', 0),
        'shortDelta': getattr(param, 'short_delta', 0),
        'longDelta': getattr(param, 'long_delta', 0),
        'nCall': getattr(param, 'n_call', 0),
        'nPut': getattr(param, 'n_put', 0),
        'mCall': getattr(param, 'm_call', 0),
        'mPut': getattr(param, 'm_put', 0),
        'units': getattr(param, 'units', 0),
        'rollDTE': getattr(param, 'roll_dte', 0),
        'gammaScalp': int(getattr(param, 'enable_gamma_scalp', False)),
        'gammaDeltaThresh': getattr(param, 'gamma_scalp_delta_thresh', 0),
        'profitTake': int(getattr(param, 'enable_profit_take', False)),
        'lossCut': int(getattr(param, 'enable_loss_cut', False)),
    }
    for key, val in mapping.items():
        if key in row:
            row[key] = round_float(val) if isinstance(val, float) else val
    return row


def add_summary_csv_row(param=None, annualReturn=0, maxDrawdown=0,
                        taxRate=0, commRate=0, totalOptGain=0, totalStkGain=0,
                        expectedPnL=0, numRolls=0, numGammaHedges=0):
    """Add a summary row to the summary CSV file."""
    row = init_csv_row(param, Summary_fields)
    add_val(row, 'annualReturn', f'{round_float(annualReturn)}%')
    add_val(row, 'maxDrawdown', f'{round_float(maxDrawdown)}%')
    add_val(row, 'taxRate', f'{round_float(taxRate)}%')
    add_val(row, 'commRate', f'{round_float(commRate)}%')
    add_val(row, 'totalOptGain', totalOptGain)
    add_val(row, 'totalStkGain', totalStkGain)
    add_val(row, 'expectedPnL', expectedPnL)
    add_val(row, 'numRolls', numRolls)
    add_val(row, 'numGammaHedges', numGammaHedges)
    param.fixed_param.summary_writer.writerow(row)


def add_monthly_csv_row(param=None, date='', action='buy', entType='stock',
                        stkShares=0, stkOpenPrice=0, stkPrice=0,
                        contracts=0, optOpenDate='', optOpenPrice=0, optPrice=0,
                        expDate='', strike=0, stkGain=0, optGain=0,
                        legRole='', reason='', interpolated=0,
                        structurePnL=0, structurePnLPct=0,
                        netDelta=0, netGamma=0, netTheta=0, expectedPnL=0,
                        stat=None, price_group=None):
    """Add a detail transaction / mark-to-market row."""
    monthly_fields = getattr(
        param.fixed_param.monthly_writer, 'fieldnames', Monthly_fields)
    row = init_csv_row(param, monthly_fields)
    add_val(row, 'date', date)
    add_val(row, 'action', action)
    add_val(row, 'entType', entType)
    add_val(row, 'legRole', legRole)
    add_val(row, 'reason', reason)

    add_val(row, 'stkShares', stkShares)
    add_val(row, 'stkOpenPrice', stkOpenPrice)
    add_val(row, 'stkPrice', stkPrice)

    add_val(row, 'contracts', round_float(contracts, digits=0))
    add_val(row, 'optOpenDate', optOpenDate)
    add_val(row, 'optOpenPrice', optOpenPrice)
    add_val(row, 'optPrice', optPrice)
    add_val(row, 'strike', strike)
    add_val(row, 'expDate', expDate)
    add_val(row, 'interpolated', interpolated)

    add_val(row, 'stkGain', stkGain)
    add_val(row, 'optGain', optGain)
    add_val(row, 'structurePnL', structurePnL)
    add_val(row, 'structurePnLPct',
            f'{round_float(structurePnLPct * 100)}%' if structurePnLPct else 0)
    add_val(row, 'netDelta', netDelta)
    add_val(row, 'netGamma', netGamma)
    add_val(row, 'netTheta', netTheta)
    add_val(row, 'expectedPnL', expectedPnL)

    add_val(row, 'totalCalls', stat.total_call_contracts())
    add_val(row, 'totalPuts', stat.total_put_contracts())
    add_val(row, 'totalShares', stat.total_shares())
    add_val(row, 'totalStkGain',
            getattr(stat, 'total_stock_capital_gain',
                    stat.total_capital_gain - stat.total_put_capital_gain
                    - stat.total_call_capital_gain))
    add_val(row, 'totalPutGain', stat.total_put_capital_gain)
    add_val(row, 'totalCallGain', stat.total_call_capital_gain)
    add_val(row, 'totalOptGain',
            stat.total_put_capital_gain + stat.total_call_capital_gain)
    add_val(row, 'totalTax', stat.total_tax)
    add_val(row, 'totalComm', stat.total_commission)
    add_val(row, 'totalGain', stat.total_capital_gain)

    nlv, opt_nlv = stat.get_balance(price_group)
    add_val(row, 'optNLV', opt_nlv)
    stock_nlv = nlv - opt_nlv
    add_val(row, 'stockNLV', stock_nlv)
    cash = getattr(stat, 'cash', 0)
    add_val(row, 'cash', cash)
    add_val(row, 'TtlNLV', nlv + cash)

    add_val(row, 'maxDrawDPeak', stat.max_drawdown_peak)
    add_val(row, 'maxDDPeakDate', stat.max_drawdown_peak_date)
    add_val(row, 'maxDrawdown', f'{round_float(stat.max_drawdown)}%')
    ttl_nlv = nlv + cash
    if stat.cur_peak == 0:
        add_val(row, 'curDrawdown', 0)
    else:
        dd = 1 - ttl_nlv / stat.cur_peak
        add_val(row, 'curDrawdown', f'{round_float(dd)}%')
    add_val(row, 'curPeak', stat.cur_peak)
    add_val(row, 'curPeakDate', stat.cur_peak_date)

    param.fixed_param.monthly_writer.writerow(row)


# ============================================================================
# Yield Data Functions
# ============================================================================

def load_yield_data(yield_file_path):
    """Load treasury yield and dividend yield data from CSV file into memory.
    
    Args:
        yield_file_path: Path to the yield curve CSV file
    """
    global YieldData, DivYieldData, YieldTenors
    global _risk_free_rate_cache, _dividend_yield_cache
    try:
        df = pd.read_csv(yield_file_path)
        df['Date'] = pd.to_datetime(df['Date'], format='mixed')
        # Convert treasury yields from percentages to decimals (e.g., 4.4% -> 0.044).
        YieldTenors = [(col, years) for col, years in TREASURY_TENOR_YEARS if col in df.columns]
        for col, _ in YieldTenors:
            df[col] = df[col] / 100.0

        if 'Div Yield' in df.columns:
            df['Div Yield'] = df['Div Yield'] / 100.0
            DivYieldData = df.set_index('Date')['Div Yield']
            print(f"Loaded dividend yield data ({DivYieldData.min():.3f}-{DivYieldData.max():.3f})",
                  file=sys.stderr)
        else:
            DivYieldData = None
            print("Warning: 'Div Yield' column not found; dividend yield assumed 0",
                  file=sys.stderr)

        if not YieldTenors:
            raise Exception("No treasury yield tenor columns found in yield file")

        YieldData = df.set_index('Date')
        _risk_free_rate_cache = {}
        _dividend_yield_cache = {}
        print(f"Loaded {len(YieldData)} yield curve records from {yield_file_path}",
              file=sys.stderr)
    except Exception as e:
        print(f"Error loading yield data from {yield_file_path}: {e}",
              file=sys.stderr)
        YieldData = None
        DivYieldData = None
        YieldTenors = []


def get_risk_free_rate(date, time_to_expire_in_years=None):
    """Get the treasury yield matching the option tenor for a given date.
    
    Args:
        date: Date to get rate for
        time_to_expire_in_years: Option tenor in years. If omitted, uses the
            shortest available tenor.
    
    Returns:
        Risk-free rate as decimal (e.g., 0.05 for 5%)
    """
    global YieldData, YieldTenors, _risk_free_rate_cache
    if YieldData is None:
        raise Exception("Yield data not loaded")
    if not YieldTenors:
        raise Exception("Yield tenor metadata not loaded")

    try:
        date_series = pd.Timestamp(date).normalize()
        target_tenor = 0 if time_to_expire_in_years is None else max(time_to_expire_in_years, 0)
        cache_key = (date_series.value, round(target_tenor, 8))
        cached_value = _risk_free_rate_cache.get(cache_key)
        if cached_value is not None:
            return cached_value

        candidate_dates = YieldData.index[YieldData.index <= date_series]
        if len(candidate_dates) == 0:
            candidate_dates = YieldData.index

        tenor_candidates = sorted(
            YieldTenors,
            key=lambda item: (abs(item[1] - target_tenor), item[1] < target_tenor)
        )

        for candidate_date in reversed(candidate_dates):
            row = YieldData.loc[candidate_date]
            for tenor_col, tenor_years in tenor_candidates:
                value = row[tenor_col]
                if pd.notna(value) and math.isfinite(float(value)):
                    rate = float(value)
                    _risk_free_rate_cache[cache_key] = rate
                    return rate

        raise Exception(f"No valid yield found on or before {date_series}")
    except Exception as e:
        print(f"Error getting risk-free rate for {date}: {e}", file=sys.stderr)
        raise Exception(f"Error getting risk-free rate for {date}: {e}")


# ============================================================================
# Dividend Yield Functions
# ============================================================================

DEFAULT_DIVIDEND_YIELD = 0.0


def get_dividend_yield(date):
    """Get dividend yield for a given date from loaded yield data.

    Returns:
        Dividend yield as decimal (e.g., 0.013 for 1.3%). When no series is
        loaded or the lookup fails, returns 0.
    """
    global DivYieldData, _dividend_yield_cache
    if DivYieldData is None:
        return 0.0

    try:
        date_ts = pd.Timestamp(date).normalize()
        cache_key = date_ts.value
        cached_value = _dividend_yield_cache.get(cache_key)
        if cached_value is not None:
            return cached_value
        closest_date = DivYieldData.index[DivYieldData.index <= date_ts].max()
        value = float(DivYieldData.loc[closest_date])
        if not math.isfinite(value):
            _dividend_yield_cache[cache_key] = 0.0
            return 0.0
        _dividend_yield_cache[cache_key] = value
        return value
    except Exception:
        return 0.0


SPLIT_MERGE_GAP_THRESHOLD = 1.7
SPLIT_MERGE_PRICE_COLUMNS = [
    'UnderlyingPrice',
    'StrikePrice',
    'BidPrice',
    'AskPrice',
    'LastPrice',
]


def _detect_split_or_merge_ratio(older_price, newer_price):
    """Return an integer split/merge ratio and adjustment factor if detected."""
    if (not math.isfinite(older_price)) or (not math.isfinite(newer_price)):
        return None
    if older_price <= 0 or newer_price <= 0:
        return None

    gap_ratio = max(older_price, newer_price) / min(older_price, newer_price)
    if gap_ratio < SPLIT_MERGE_GAP_THRESHOLD:
        return None

    integer_ratio = max(2, int(round(gap_ratio)))
    if older_price > newer_price:
        return {
            'ratio_text': f'{integer_ratio}:1 split',
            'adjustment_factor': 1.0 / integer_ratio,
        }

    return {
        'ratio_text': f'1:{integer_ratio} merge',
        'adjustment_factor': float(integer_ratio),
    }


def adjust_prices_for_splits_and_merges(df):
    """Normalize historical prices to the latest share count basis.

    The raw option files occasionally contain an abrupt discontinuity when the
    underlying undergoes a stock split or reverse split. When the price ratio
    between consecutive trading days is large enough to indicate a corporate
    action, rewrite all prior stock, strike, and option price columns so the
    entire history is expressed on today's share basis.
    """
    if df.empty or 'DataDate' not in df.columns or 'UnderlyingPrice' not in df.columns:
        return df

    adjusted_df = df.copy()
    adjusted_df['_original_index'] = adjusted_df.index
    adjusted_df['_row_order'] = range(len(adjusted_df))
    daily_prices = adjusted_df[['DataDate', 'UnderlyingPrice']].drop_duplicates(
        subset='DataDate', keep='first')
    daily_prices = daily_prices.sort_values('DataDate').reset_index(drop=True)
    if daily_prices.empty:
        return adjusted_df

    daily_prices['adjustment_factor'] = 1.0
    daily_prices['commFactor'] = 1.0
    cumulative_factor = 1.0

    for idx in range(len(daily_prices) - 2, -1, -1):
        older_price = float(daily_prices.at[idx, 'UnderlyingPrice'])
        newer_date = daily_prices.at[idx + 1, 'DataDate']
        newer_price = float(daily_prices.at[idx + 1, 'UnderlyingPrice'])

        event = _detect_split_or_merge_ratio(older_price, newer_price)
        if event is not None:
            cumulative_factor *= event['adjustment_factor']
            print(
                'Detected split/merge on '
                f"{pd.Timestamp(newer_date).strftime('%Y-%m-%d')}: "
                f"{event['ratio_text']}",
                file=sys.stderr,
            )

        daily_prices.at[idx, 'adjustment_factor'] = cumulative_factor
        daily_prices.at[idx, 'commFactor'] = cumulative_factor

    adjusted_df = adjusted_df.merge(
        daily_prices[['DataDate', 'adjustment_factor', 'commFactor']],
        on='DataDate',
        how='left',
    )

    for col in SPLIT_MERGE_PRICE_COLUMNS:
        if col in adjusted_df.columns:
            adjusted_df[col] = adjusted_df[col] * adjusted_df['adjustment_factor']

    adjusted_df.sort_values('_row_order', kind='stable', inplace=True)
    adjusted_df.set_index('_original_index', inplace=True)
    adjusted_df.index.name = df.index.name
    adjusted_df.drop(columns=['adjustment_factor', '_row_order'], inplace=True)

    return adjusted_df


def get_commission_factor(price_group):
    """Return the commission adjustment factor for a row or daily group."""
    if price_group is None:
        return 1.0

    if isinstance(price_group, pd.DataFrame):
        if price_group.empty or 'commFactor' not in price_group.columns:
            return 1.0
        return float(price_group['commFactor'].iloc[0])

    if isinstance(price_group, pd.Series):
        return float(price_group.get('commFactor', 1.0))

    return float(getattr(price_group, 'commFactor', 1.0))


def get_stock_commission_per_share(param, price_group):
    """Return the split-adjusted stock commission for the current day."""
    return param.fixed_param.stock_comm_per_share * \
        get_commission_factor(price_group)


def get_option_commission_per_contract(param, price_group):
    """Return the split-adjusted option commission for the current day."""
    return param.fixed_param.opt_comm_per_contract * \
        get_commission_factor(price_group)


def sanitize_option_price_for_iv(option_type, stock_price, strike, time_to_expire,
                                 risk_free_rate, dividend_yield, option_price):
    """Clamp an observed price inside no-arbitrage bounds before IV inversion."""
    discounted_stock = stock_price * math.exp(-dividend_yield * time_to_expire)
    discounted_strike = strike * math.exp(-risk_free_rate * time_to_expire)

    if option_type == 'p':
        lower_bound = max(discounted_strike - discounted_stock, 0.0)
        upper_bound = discounted_strike
    else:
        lower_bound = max(discounted_stock - discounted_strike, 0.0)
        upper_bound = discounted_stock

    if upper_bound <= lower_bound:
        return None

    eps = max(1e-6, upper_bound * 1e-6)
    return min(max(option_price, lower_bound + eps), upper_bound - eps)


# ============================================================================
# Option Pricing Functions
# ============================================================================

def _rank_similar_candidates(search_space, opt_type, data_date, exp_ts,
                             target_strike, same_day_only=False):
    """Rank candidate quotes by day, expiry, then strike distance."""
    same_type = search_space[search_space['PutCall'] == opt_type].copy()
    if same_type.empty:
        return None

    same_type['mid_price'] = (same_type['BidPrice'] + same_type['AskPrice']) / 2
    same_type = same_type[
        same_type['mid_price'].notna() & (same_type['mid_price'] > 0)
    ]
    if same_type.empty:
        return None

    exp_norm = same_type['ExpirationDate'].dt.normalize()
    same_type['exp_distance'] = (exp_norm - exp_ts).dt.days.abs()
    same_type['strike_distance'] = (same_type['StrikePrice'] - target_strike).abs()

    if same_day_only:
        return same_type.sort_values(
            ['exp_distance', 'strike_distance', 'ExpirationDate', 'StrikePrice']
        )

    date_norm = same_type['DataDate'].dt.normalize()
    same_type['day_distance'] = (date_norm - data_date).dt.days.abs()
    same_type['is_future_date'] = date_norm > data_date
    return same_type.sort_values(
        ['day_distance', 'is_future_date', 'exp_distance', 'strike_distance',
         'ExpirationDate', 'StrikePrice']
    )


def _similar_option_candidates(group, opt_type, exp_date, target_strike,
                               all_data=None, max_lookback_days=30,
                               max_forward_days=730):
    """Return nearby same-type option quotes ordered by day, expiry, then strike."""
    try:
        data_date = pd.Timestamp(group['DataDate'].iloc[0]).normalize()
        exp_ts = pd.Timestamp(exp_date).normalize()

        # Most missing quotes can be synthesized from the same day's chain.
        same_day_candidates = _rank_similar_candidates(
            group, opt_type, data_date, exp_ts, target_strike, same_day_only=True)
        if same_day_candidates is not None and not same_day_candidates.empty:
            return same_day_candidates

        if all_data is None:
            return same_day_candidates

        window_start = data_date - pd.Timedelta(days=max_lookback_days)
        window_end = data_date + pd.Timedelta(days=max_forward_days)
        search_space = all_data.loc[
            (all_data['PutCall'] == opt_type) &
            (all_data['DataDate'] >= window_start) &
            (all_data['DataDate'] <= window_end),
            ['PutCall', 'DataDate', 'ExpirationDate', 'StrikePrice',
             'BidPrice', 'AskPrice', 'UnderlyingPrice']
        ]
        if search_space.empty:
            return None

        return _rank_similar_candidates(
            search_space, opt_type, data_date, exp_ts, target_strike)

    except Exception as e:
        print(f"Error finding closest option IV: {e}", file=sys.stderr)
        return None


def find_closest_option_iv(group, opt_type, exp_date, target_strike,
                           all_data=None, max_lookback_days=30):
    """Find implied volatility from nearby options across strikes, expiries, and prior days."""
    candidates = _similar_option_candidates(
        group, opt_type, exp_date, target_strike,
        all_data=all_data, max_lookback_days=max_lookback_days)
    if candidates is None or candidates.empty:
        return None

    for row in candidates.itertuples(index=False):
        mid_price = row.mid_price
        strike = row.StrikePrice
        quote_date = pd.Timestamp(row.DataDate).normalize()
        row_exp = pd.Timestamp(row.ExpirationDate).normalize()
        quote_stock_price = row.UnderlyingPrice
        dividend_yield = get_dividend_yield(quote_date)
        days_to_exp = (row_exp - quote_date).days
        time_to_expire = max(days_to_exp / 365.25, 1e-6)
        risk_free_rate = get_risk_free_rate(quote_date, time_to_expire)

        try:
            iv_price = sanitize_option_price_for_iv(
                opt_type, quote_stock_price, strike, time_to_expire,
                risk_free_rate, dividend_yield, mid_price)
            if iv_price is None:
                continue
            implied_vol = euro_implied_vol(
                opt_type, fs=quote_stock_price, x=strike,
                t=time_to_expire, r=risk_free_rate, q=dividend_yield, cp=iv_price)
            if (not math.isfinite(implied_vol)) or implied_vol <= 0:
                continue
            return implied_vol
        except Exception:
            continue

    return None


def calculate_option_price_black_scholes_details(group, opt_type, strike, exp_date,
                                                 all_data=None):
    """Estimate a missing option using nearby IV and the treasury curve.
    
    Args:
        group: DataFrame group for the current date
        opt_type: 'p' for put, 'c' for call
        strike: Strike price
        exp_date: Expiration date
    
    Returns:
        Dictionary with price, delta, implied_vol, risk_free_rate, and dividend_yield.
    """
    stock_price = group['UnderlyingPrice'].iloc[0]
    data_date = group['DataDate'].iloc[0]
    days_to_exp = (pd.Timestamp(exp_date) - pd.Timestamp(data_date)).days
    time_to_expire = days_to_exp / 365.25

    # Already expired: option has no value
    if time_to_expire <= 0:
        return {
            'price': 0.0,
            'delta': 0.0,
            'implied_vol': 0.0,
            'risk_free_rate': 0.0,
            'dividend_yield': get_dividend_yield(data_date),
        }

    # Get risk-free rate from treasury yield data
    risk_free_rate = get_risk_free_rate(data_date, time_to_expire)
    dividend_yield = get_dividend_yield(data_date)

    candidates = _similar_option_candidates(
        group, opt_type, exp_date, strike, all_data=all_data)
    if candidates is None or candidates.empty:
        raise Exception(f"No similar option found for {opt_type} {strike} exp {exp_date}")

    for row in candidates.itertuples(index=False):
        quote_date = pd.Timestamp(row.DataDate).normalize()
        row_exp = pd.Timestamp(row.ExpirationDate).normalize()
        quote_stock_price = row.UnderlyingPrice
        quote_dividend_yield = get_dividend_yield(quote_date)
        quote_time_to_expire = max((row_exp - quote_date).days / 365.25, 1e-6)
        quote_risk_free_rate = get_risk_free_rate(quote_date, quote_time_to_expire)

        try:
            iv_price = sanitize_option_price_for_iv(
                opt_type, quote_stock_price, row.StrikePrice, quote_time_to_expire,
                quote_risk_free_rate, quote_dividend_yield, row.mid_price)
            if iv_price is None:
                continue
            implied_vol = euro_implied_vol(
                opt_type, fs=quote_stock_price, x=row.StrikePrice,
                t=quote_time_to_expire, r=quote_risk_free_rate,
                q=quote_dividend_yield, cp=iv_price)
            if (not math.isfinite(implied_vol)) or implied_vol <= 0:
                continue

            value, delta, gamma, theta, vega, rho = merton(
                opt_type, fs=stock_price, x=strike, t=time_to_expire,
                r=risk_free_rate, q=dividend_yield, v=implied_vol)
            if not math.isfinite(value) or not math.isfinite(delta):
                continue

            return {
                'price': value,
                'delta': delta,
                'implied_vol': implied_vol,
                'risk_free_rate': risk_free_rate,
                'dividend_yield': dividend_yield,
            }
        except Exception:
            continue

    raise Exception(
        f"No usable similar option found for {opt_type} {strike} exp {exp_date}")


def calculate_option_price_black_scholes(group, opt_type, strike, exp_date, all_data=None):
    """Calculate option price using Black-Scholes formula when option data is missing."""
    try:
        return calculate_option_price_black_scholes_details(
            group, opt_type, strike, exp_date, all_data=all_data)['price']

    except Exception as e:
        failure_key = (opt_type, round_float(float(strike)),
                       pd.Timestamp(exp_date).strftime("%Y-%m-%d"))
        if failure_key not in _logged_bs_failures:
            print(f"Error calculating Black-Scholes price: {e}", file=sys.stderr)
            _logged_bs_failures.add(failure_key)
        return 0.0


# ============================================================================
# Pricing Helper Functions
# ============================================================================

def get_sell_price(param, put_info) -> (float, float):
    """Get the sell price for an option.
    
    Args:
        param: Parameter object with fixed_param.opt_slippage_percent
        put_info: Option info dictionary with BidPrice and AskPrice
    
    Returns:
        Tuple of (slippage-adjusted price, mid price)
    """
    mid = (put_info['BidPrice'] + put_info['AskPrice']) / 2
    return max(mid * (1 - param.fixed_param.opt_slippage_percent), 0), mid


def get_buy_price(param, opt_info) -> (float, float):
    """Get the buy price for an option.
    
    Args:
        param: Parameter object with fixed_param.opt_slippage_percent
        opt_info: Option info dictionary with BidPrice and AskPrice
    
    Returns:
        Tuple of (slippage-adjusted price, mid price)
    """
    mid = (opt_info['BidPrice'] + opt_info['AskPrice']) / 2
    return max(mid * (1 + param.fixed_param.opt_slippage_percent), 0), mid


def delta_iv(param, date, stock_price, pos):
    """Calculate delta and implied volatility for an option position.

    Returns:
        Tuple of (delta, implied_vol)
    """
    g = greeks_iv(param, date, stock_price, pos)
    return g['delta'], g['implied_vol']


def greeks_iv(param, date, stock_price, pos, option_mid=None):
    """Calculate delta, gamma, theta and IV for an option position.

    Merton theta is annualized dollar theta per share; callers may divide by
    365.25 for a daily approximation.
    """
    days = (pd.Timestamp(pos.exp_date) - pd.Timestamp(date)).days
    time_to_expire_in_years = max(days / 365.25, 1e-6)
    risk_free_rate = get_risk_free_rate(date, time_to_expire_in_years)
    dividend_yield = get_dividend_yield(date)
    mid = option_mid if option_mid is not None else pos.org_price

    try:
        iv_price = sanitize_option_price_for_iv(
            pos.opt_type, stock_price, pos.strike, time_to_expire_in_years,
            risk_free_rate, dividend_yield, mid)
        if iv_price is None:
            raise Exception('price outside bounds')
        implied_vol = euro_implied_vol(
            pos.opt_type, fs=stock_price, x=pos.strike, t=time_to_expire_in_years,
            r=risk_free_rate, q=dividend_yield, cp=iv_price)
        if (not math.isfinite(implied_vol)) or implied_vol <= 0:
            raise Exception('Non-finite implied volatility')
    except Exception:
        if getattr(pos, 'implied_vol', 0) and pos.implied_vol > 0:
            implied_vol = pos.implied_vol
        else:
            raise

    value, delta, gamma, theta, vega, rho = merton(
        pos.opt_type, fs=stock_price, x=pos.strike, t=time_to_expire_in_years,
        r=risk_free_rate, q=dividend_yield, v=implied_vol)

    return {
        'value': value,
        'delta': delta,
        'gamma': gamma,
        'theta': theta,
        'vega': vega,
        'implied_vol': implied_vol,
        'risk_free_rate': risk_free_rate,
        'dividend_yield': dividend_yield,
    }
