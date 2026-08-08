#!/usr/bin/env python3
"""Portfolio state: stock + option positions, cost basis, tax, NLV."""

from __future__ import annotations

import pandas as pd

from OptionPos import CONTRACT_SCALE
from StockPos import StockPosition
from PriceDataUtil import calculate_option_price_black_scholes
from Util import round_float


class OptionQuote:
    """Compact quote object for hot-path option lookups."""

    __slots__ = (
        'BidPrice', 'AskPrice', 'StrikePrice',
        'ExpirationDate', 'DataDate', 'UnderlyingPrice',
    )

    def __init__(self, row) -> None:
        self.BidPrice = row.BidPrice
        self.AskPrice = row.AskPrice
        self.StrikePrice = row.StrikePrice
        self.ExpirationDate = pd.Timestamp(row.ExpirationDate)
        self.DataDate = pd.Timestamp(row.DataDate)
        self.UnderlyingPrice = row.UnderlyingPrice

    def __getitem__(self, key):
        return getattr(self, key)


class DayOptionLookup:
    """Indexed intraday option-chain view for fast strike/expiry lookups."""

    def __init__(self, price_group) -> None:
        self.group = price_group
        self.date = pd.Timestamp(price_group['DataDate'].iloc[0])
        self.stock_price = price_group['UnderlyingPrice'].iloc[0]
        self._exact = {}
        exp_buckets = {}

        for row in price_group.itertuples(index=False):
            quote = OptionQuote(row)
            exp_date = quote.ExpirationDate
            exact_key = (row.PutCall, quote.StrikePrice, exp_date)
            self._exact[exact_key] = quote
            exp_buckets.setdefault((row.PutCall, exp_date), []).append(
                (quote.StrikePrice, quote)
            )

        self._by_exp = {
            key: self._finalize_bucket(bucket)
            for key, bucket in exp_buckets.items()
        }

    @staticmethod
    def _finalize_bucket(bucket):
        bucket.sort(key=lambda item: item[0])
        strikes = [strike for strike, _ in bucket]
        quotes = [quote for _, quote in bucket]
        return strikes, quotes

    def find_exact(self, opt_type, strike, exp_date):
        return self._exact.get((opt_type, float(strike), pd.Timestamp(exp_date)))


class PositionsAndStats:
    """Tracks positions, P/L, tax, commissions, and W-structure state."""

    def __init__(self) -> None:
        self.all_data = None
        self.stocks = []
        self.cash = 0.0
        self.annual_capital_gain = 0.0
        self.total_tax = 0.0
        self.opt_list = []

        self.initial_price = 0.0
        self.cur_peak = 0.0
        self.cur_peak_date = None
        self.max_drawdown_peak = None
        self.max_drawdown_peak_date = None
        self.max_drawdown = 0.0

        self.total_capital_gain = 0.0
        self.total_put_capital_gain = 0.0
        self.total_call_capital_gain = 0.0
        self.total_stock_capital_gain = 0.0
        self.total_commission = 0.0
        self.num_data_missing = 0

        # W structure bookkeeping
        self.structure_id = 0
        self.structure_entry_value = 0.0   # mark of options at open (signed)
        self.structure_entry_cash = 0.0    # net cash paid(+) / received(-) to open
        self.structure_open_date = None
        self.structure_open_spot = 0.0
        self.num_rolls = 0
        self.num_gamma_hedges = 0
        self.last_expected_pnl = 0.0
        self.trades = []
        self.gamma_scalping_active = False

        self._price_lookup_group_id = None
        self._price_lookup = None
        self._balance_cache_key = None
        self._balance_cache_value = None

    def print_fields(self):
        for attr in sorted(self.__dict__):
            if attr in ['all_data', 'stocks', 'opt_list', 'trades'] or attr.startswith('_'):
                continue
            print(f"{attr}: {round_float(self.__dict__[attr])}")

    def get_price_lookup(self, price_group):
        group_id = id(price_group)
        if self._price_lookup_group_id != group_id:
            self._price_lookup = DayOptionLookup(price_group)
            self._price_lookup_group_id = group_id
            self._balance_cache_key = None
            self._balance_cache_value = None
        return self._price_lookup

    def invalidate_balance_cache(self):
        self._balance_cache_key = None
        self._balance_cache_value = None

    def _balance_key(self, price_group):
        group_date = pd.Timestamp(price_group['DataDate'].iloc[0]).value
        opt_state = tuple(sorted(
            (opt.opt_type,
             float(opt.strike),
             pd.Timestamp(opt.exp_date).value if opt.exp_date is not None else None,
             float(opt.num_contracts))
            for opt in self.opt_list
        ))
        return (group_date, float(self.total_shares()), opt_state)

    def mark_option(self, opt, price_group) -> float:
        """Mid mark for one option position (per share).

        Prefer Black-Scholes with the position's last known IV (fast). Fall back
        to the live chain mid only when IV is unavailable.
        """
        target_date = pd.Timestamp(price_group['DataDate'].iloc[0])
        spot = float(price_group['UnderlyingPrice'].iloc[0])
        if opt.exp_date is not None and pd.Timestamp(opt.exp_date) <= target_date:
            if opt.opt_type == 'p':
                return max(0.0, float(opt.strike) - spot)
            return max(0.0, spot - float(opt.strike))

        if getattr(opt, 'implied_vol', 0) and opt.implied_vol > 0:
            from PriceDataUtil import get_risk_free_rate, get_dividend_yield, merton
            import math
            days = (pd.Timestamp(opt.exp_date) - target_date).days
            t = max(days / 365.25, 1e-6)
            try:
                r = get_risk_free_rate(target_date, t)
                q = get_dividend_yield(target_date)
                value, *_ = merton(
                    opt.opt_type, fs=spot, x=float(opt.strike), t=t,
                    r=r, q=q, v=float(opt.implied_vol))
                if math.isfinite(value):
                    return float(value)
            except Exception:
                pass

        price_lookup = self.get_price_lookup(price_group)
        opt_quote = price_lookup.find_exact(opt.opt_type, opt.strike, opt.exp_date)
        if opt_quote is not None:
            return (opt_quote['BidPrice'] + opt_quote['AskPrice']) / 2

        self.num_data_missing += 1
        return calculate_option_price_black_scholes(
            price_group, opt.opt_type, opt.strike, opt.exp_date,
            all_data=self.all_data)

    def get_balance(self, price_group):
        balance_key = self._balance_key(price_group)
        if self._balance_cache_key == balance_key:
            return self._balance_cache_value

        total_shares = self.total_shares()
        nlv = total_shares * float(price_group['UnderlyingPrice'].iloc[0])
        opt_nlv = 0.0
        for opt in self.opt_list:
            mid = self.mark_option(opt, price_group)
            opt_nlv += mid * opt.num_contracts * CONTRACT_SCALE
        nlv += opt_nlv
        self._balance_cache_key = balance_key
        self._balance_cache_value = (nlv, opt_nlv)
        return self._balance_cache_value

    def total_shares(self) -> float:
        return sum(position.num_shares for position in self.stocks)

    def total_put_contracts(self) -> float:
        return sum(opt.num_contracts for opt in self.opt_list if opt.opt_type == 'p')

    def total_call_contracts(self) -> float:
        return sum(opt.num_contracts for opt in self.opt_list if opt.opt_type == 'c')

    def option_nlv(self, price_group) -> float:
        return self.get_balance(price_group)[1]

    def structure_mark(self, price_group) -> float:
        """Current marked value of option legs only."""
        return self.option_nlv(price_group)

    def structure_pnl(self, price_group) -> float:
        """Unrealized + realized-vs-entry P/L of current option structure.

        Entry cash paid is positive debit / negative credit. Current mark of
        options minus entry cash equals P/L.
        """
        if not self.opt_list:
            return 0.0
        return self.structure_mark(price_group) - self.structure_entry_cash

    def structure_pnl_pct(self, price_group) -> float:
        denom = abs(self.structure_entry_cash)
        if denom < 1e-6:
            return 0.0
        return self.structure_pnl(price_group) / denom

    def net_greeks(self):
        delta = sum(opt.total_delta() for opt in self.opt_list)
        gamma = sum(opt.total_gamma() for opt in self.opt_list)
        # theta from merton is annual $; convert to daily $
        theta = sum(opt.total_theta() for opt in self.opt_list) / 365.25
        delta += self.total_shares()  # stock delta = 1 per share
        return delta, gamma, theta

    def short_min_dte(self, as_of) -> int | None:
        shorts = [o for o in self.opt_list if o.is_short()]
        if not shorts:
            return None
        return min(o.dte(as_of) for o in shorts)

    def close_stock_positions(self, num_shares, close_price):
        """Close shares HIFO; returns (capital_gain, avg_open_price)."""
        assert num_shares >= 0
        sorted_positions = sorted(self.stocks, key=lambda x: x.open_price, reverse=True)
        shares_to_close = num_shares
        capital_gain = 0.0

        for position in list(sorted_positions):
            if shares_to_close <= 0:
                break
            shares_closed = min(position.num_shares, shares_to_close)
            capital_gain += (close_price - position.open_price) * shares_closed
            position.num_shares -= shares_closed
            shares_to_close -= shares_closed
            if position.num_shares == 0 and position in self.stocks:
                self.stocks.remove(position)

        self.total_capital_gain += capital_gain
        self.total_stock_capital_gain += capital_gain
        self.annual_capital_gain += capital_gain
        avg_open = close_price if num_shares == 0 else close_price - capital_gain / num_shares
        return capital_gain, avg_open

    def open_stock(self, num_shares, open_price, open_date):
        if abs(num_shares) < 1e-9:
            return
        if num_shares > 0:
            self.stocks.append(StockPosition(
                open_price=open_price, open_date=open_date, num_shares=num_shares))
        else:
            # short stock: track as negative shares lot
            self.stocks.append(StockPosition(
                open_price=open_price, open_date=open_date, num_shares=num_shares))
        self.invalidate_balance_cache()
