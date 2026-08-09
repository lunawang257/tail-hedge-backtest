#!/usr/bin/env python3
"""W tail-hedge strategy: short near-ATM premium + long OTM wings.

Profit/loss vs spot looks like a letter 'W': wings profit on large moves while
short legs collect theta when the market is quiet.
"""

from __future__ import annotations

import sys

import pandas as pd

from OptionPos import (
    OptPosition, CONTRACT_SCALE,
    LEG_SHORT_CALL, LEG_SHORT_PUT, LEG_LONG_CALL, LEG_LONG_PUT,
)
from ChainInterpolate import find_option_by_delta
from ExpectedPnL import expected_structure_pnl
from PriceDataUtil import (
    get_buy_price, get_sell_price,
    get_option_commission_per_contract, get_stock_commission_per_share,
    greeks_iv, add_monthly_csv_row, add_summary_csv_row,
)
from Util import round_float


ANNUAL_CAPITAL_LOSS_DEDUCTION = 0


def _log(debug, msg):
    if debug:
        print(msg, file=sys.stderr)


def update_greeks(param, date, stock_price, stat):
    """Refresh per-contract greeks on all open option legs."""
    for opt in stat.opt_list:
        if opt.exp_date is not None and pd.Timestamp(opt.exp_date) <= pd.Timestamp(date):
            opt.per_contract_delta = -1.0 if opt.opt_type == 'p' else 1.0
            # near/at expiry intrinsic delta approx; gamma/theta ~ 0
            if opt.opt_type == 'p':
                opt.per_contract_delta = -1.0 if stock_price < opt.strike else 0.0
            else:
                opt.per_contract_delta = 1.0 if stock_price > opt.strike else 0.0
            opt.per_contract_gamma = 0.0
            opt.per_contract_theta = 0.0
            continue
        try:
            g = greeks_iv(param, date, stock_price, opt)
            opt.per_contract_delta = g['delta']
            opt.per_contract_gamma = g['gamma']
            opt.per_contract_theta = g['theta']
            opt.implied_vol = g['implied_vol']
        except Exception as e:
            _log(True, f'{date.date()}: greeks failed for {opt}: {e}')


def year_end_tax(param, date, stat, debug=False):
    """Pay capital-gains tax on positive annual gains; carry losses forward."""
    gain = stat.annual_capital_gain
    if gain > 0:
        tax = gain * param.fixed_param.capital_gain_tax
        stat.total_tax += tax
        stat.cash -= tax
        _log(debug, f'{date.date()}: tax ${tax:.0f} on gain ${gain:.0f}')
        stat.annual_capital_gain = 0.0
    else:
        deductible = max(gain, -ANNUAL_CAPITAL_LOSS_DEDUCTION)
        # Keep undeducted loss as carry-forward in annual_capital_gain
        stat.annual_capital_gain = gain - deductible
        if ANNUAL_CAPITAL_LOSS_DEDUCTION == 0:
            # No annual deduction applied; retain full loss carry
            pass


def calc_drawdown(stat, date_str, ttl_nlv):
    if ttl_nlv > stat.cur_peak:
        stat.cur_peak = ttl_nlv
        stat.cur_peak_date = date_str
    if stat.cur_peak > 0:
        dd = 1 - ttl_nlv / stat.cur_peak
        if dd > stat.max_drawdown:
            stat.max_drawdown = dd * 100
            stat.max_drawdown_peak = stat.cur_peak
            stat.max_drawdown_peak_date = stat.cur_peak_date


def _quote_prices(param, quote, is_buy):
    """Return (trade_price_with_slip, mid) for buy or sell."""
    if is_buy:
        return get_buy_price(param, quote)
    return get_sell_price(param, quote)


def _open_leg(param, date, group, stat, quote, num_contracts, leg_role, debug=False):
    """Open one option leg. num_contracts > 0 long, < 0 short."""
    if quote is None or num_contracts == 0:
        return 0.0
    is_buy = num_contracts > 0
    trade_px, mid = _quote_prices(param, quote, is_buy=is_buy)
    opt_comm = get_option_commission_per_contract(param, group)
    # Per-share open_price includes commission allocated per share
    signed = 1 if is_buy else -1
    # Cash: buy pays (px*100 + comm)*n ; sell receives (px*100 - comm)*|n|
    n = abs(num_contracts)
    cash_per_contract = trade_px * CONTRACT_SCALE + (opt_comm if is_buy else -opt_comm)
    cash_flow = signed * cash_per_contract * n  # + outflow for buys

    pos = OptPosition(
        opt_type=quote['PutCall'],
        strike=quote['StrikePrice'],
        exp_date=pd.Timestamp(quote['ExpirationDate']),
        num_contracts=num_contracts,
        open_date=date,
        open_price=trade_px + (opt_comm / CONTRACT_SCALE) * (1 if is_buy else -1),
        org_price=mid,
        delta=quote.get('delta', 0),
        gamma=quote.get('gamma', 0),
        theta=quote.get('theta', 0),
        implied_vol=quote.get('implied_vol', 0),
        leg_role=leg_role,
        open_stock_price=float(group['UnderlyingPrice'].iloc[0]),
        structure_id=stat.structure_id,
    )
    # Refresh greeks from mid if missing
    if not pos.per_contract_delta:
        try:
            g = greeks_iv(param, date, pos.open_stock_price, pos)
            pos.per_contract_delta = g['delta']
            pos.per_contract_gamma = g['gamma']
            pos.per_contract_theta = g['theta']
            pos.implied_vol = g['implied_vol']
        except Exception:
            pass

    stat.opt_list.append(pos)
    stat.cash -= cash_flow
    stat.total_commission += opt_comm * n
    stat.invalidate_balance_cache()

    date_str = date.strftime('%Y-%m-%d')
    add_monthly_csv_row(
        param=param, date=date_str,
        action='buy' if is_buy else 'sell',
        entType='call' if pos.opt_type == 'c' else 'put',
        legRole=leg_role, reason='open',
        stkPrice=pos.open_stock_price,
        contracts=num_contracts,
        optOpenDate=date_str,
        optOpenPrice=pos.open_price,
        optPrice=trade_px,
        strike=pos.strike,
        expDate=pos.exp_date.strftime('%Y-%m-%d'),
        interpolated=1 if quote.get('interpolated') else 0,
        stat=stat, price_group=group,
    )
    _log(debug, f'{date_str}: open {pos} px={trade_px:.3f} cash_flow={cash_flow:.0f}')
    return cash_flow  # positive = debit paid


def _trade_mid(quote, is_long, slip_pct):
    """Per-share trade price after slippage (long pays ask-ish, short receives bid-ish)."""
    mid = float(quote.get('mid') or
                0.5 * (float(quote['BidPrice']) + float(quote['AskPrice'])))
    if is_long:
        return mid * (1.0 + slip_pct)
    return mid * (1.0 - slip_pct)


def choose_wing_counts(param, short_call_q, short_put_q, long_call_q, long_put_q):
    """Pick (m_call, m_put) from wing_mode using quotes for one unit.

    credit: maximize equal-ish wings while long debit stays below short credit
            by credit_buffer (longs cost slightly less than short premium).
    theta:  maximize wings while net structure theta stays >= theta_threshold.
    fixed:  use param.m_call / param.m_put.
    """
    n_c, n_p = param.n_call, param.n_put
    if param.wing_mode == 'fixed':
        return param.m_call, param.m_put

    slip = param.fixed_param.opt_slippage_percent
    sc = _trade_mid(short_call_q, is_long=False, slip_pct=slip)
    sp = _trade_mid(short_put_q, is_long=False, slip_pct=slip)
    lc = _trade_mid(long_call_q, is_long=True, slip_pct=slip)
    lp = _trade_mid(long_put_q, is_long=True, slip_pct=slip)

    max_m = max(n_c, n_p) * max(1, param.max_wing_mult)
    # Prefer at least as many longs as shorts on each side
    m_c = n_c
    m_p = n_p
    best = (m_c, m_p)

    def net_credit(mc, mp):
        short_credit = (sc * n_c + sp * n_p) * CONTRACT_SCALE
        long_debit = (lc * mc + lp * mp) * CONTRACT_SCALE
        return short_credit - long_debit

    def net_theta(mc, mp):
        # Position theta ($/day): n * θ_pc * 100; shorts have negative n
        return (
            (-n_c) * float(short_call_q.get('theta', 0.0))
            + (-n_p) * float(short_put_q.get('theta', 0.0))
            + mc * float(long_call_q.get('theta', 0.0))
            + mp * float(long_put_q.get('theta', 0.0))
        ) * CONTRACT_SCALE

    if param.wing_mode == 'credit':
        # Largest M with longs costing slightly less than shorts
        max_long_cost = (sc * n_c + sp * n_p) * (1.0 - param.credit_buffer) * CONTRACT_SCALE
        for m in range(max(n_c, n_p), max_m + 1):
            mc = max(n_c, m)
            mp = max(n_p, m)
            long_debit = (lc * mc + lp * mp) * CONTRACT_SCALE
            if long_debit <= max_long_cost + 1e-9:
                best = (mc, mp)
            else:
                break
        # If even M=N is a debit, keep M=N (best available hedge floor)
        return best

    if param.wing_mode == 'theta':
        for m in range(max(n_c, n_p), max_m + 1):
            mc = max(n_c, m)
            mp = max(n_p, m)
            if net_theta(mc, mp) >= param.theta_threshold - 1e-9:
                best = (mc, mp)
            else:
                break
        return best

    return param.m_call, param.m_put


def open_w_structure(param, date, group, stat, debug=False):
    """Open short near-ATM + long OTM wings sized by units / wing_mode."""
    stock_price = float(group['UnderlyingPrice'].iloc[0])
    slip = param.fixed_param.opt_slippage_percent
    u = param.units

    # Resolve strikes/quotes once, then size wings (shared IV cache for speed)
    iv_cache = {}
    short_call_q = find_option_by_delta(
        group, 'c', date, stock_price, param.short_delta, param.short_dte,
        prefer_otm=True, slip_pct=slip, allow_interpolate=True, iv_cache=iv_cache)
    short_put_q = find_option_by_delta(
        group, 'p', date, stock_price, param.short_delta, param.short_dte,
        prefer_otm=True, slip_pct=slip, allow_interpolate=True, iv_cache=iv_cache)
    long_call_q = find_option_by_delta(
        group, 'c', date, stock_price, param.long_delta, param.long_dte,
        prefer_otm=True, slip_pct=slip, allow_interpolate=True, iv_cache=iv_cache)
    long_put_q = find_option_by_delta(
        group, 'p', date, stock_price, param.long_delta, param.long_dte,
        prefer_otm=True, slip_pct=slip, allow_interpolate=True, iv_cache=iv_cache)
    for role, q in [
        (LEG_SHORT_CALL, short_call_q), (LEG_SHORT_PUT, short_put_q),
        (LEG_LONG_CALL, long_call_q), (LEG_LONG_PUT, long_put_q),
    ]:
        if q is None:
            _log(True, f'{date.date()}: failed to find {role}')
            return False

    m_call, m_put = choose_wing_counts(
        param, short_call_q, short_put_q, long_call_q, long_put_q)
    quotes = [
        (short_call_q, -param.n_call * u, LEG_SHORT_CALL),
        (short_put_q, -param.n_put * u, LEG_SHORT_PUT),
        (long_call_q, m_call * u, LEG_LONG_CALL),
        (long_put_q, m_put * u, LEG_LONG_PUT),
    ]

    # Theta check (informational): prefer |θ_long| < |θ_short|
    short_theta = sum(
        abs(n) * q.get('theta', 0) * CONTRACT_SCALE
        for q, n, role in quotes if n < 0)
    long_theta = sum(
        abs(n) * q.get('theta', 0) * CONTRACT_SCALE
        for q, n, role in quotes if n > 0)
    if abs(long_theta) > abs(short_theta) + 1e-6:
        _log(debug,
             f'{date.date()}: warning long|θ|={abs(long_theta):.2f} > '
             f'short|θ|={abs(short_theta):.2f}')

    stat.structure_id += 1
    entry_cash = 0.0
    for q, n, role in quotes:
        entry_cash += _open_leg(param, date, group, stat, q, n, role, debug=debug)

    stat.structure_entry_cash = entry_cash
    stat.structure_entry_value = stat.option_nlv(group)
    stat.structure_open_date = date
    stat.structure_open_spot = stock_price
    stat.last_wing_m = (m_call, m_put)
    stat.gamma_scalping_active = False
    _log(debug,
         f'{date.date()}: opened W#{stat.structure_id} '
         f'M={m_call}/{m_put} mode={param.wing_mode} entry_cash={entry_cash:.0f}')
    return True


def _close_one_option(param, date, group, stat, opt, reason, debug=False):
    """Close a single option leg; returns capital gain.

    Cash on close = trade_px * num_contracts * 100 - commission * |contracts|
    (long sell receives less; short buy costs more). Capital gain is cash on
    close minus cost basis recorded at open.
    """
    stock_price = float(group['UnderlyingPrice'].iloc[0])
    mid = stat.mark_option(opt, group)
    is_long = opt.num_contracts > 0
    slip = param.fixed_param.opt_slippage_percent
    trade_px = max(mid * (1 - slip), 0) if is_long else max(mid * (1 + slip), 0)

    opt_comm = get_option_commission_per_contract(param, group)
    cash_change = (trade_px * opt.num_contracts * CONTRACT_SCALE
                   - opt_comm * abs(opt.num_contracts))
    capital_gain = cash_change - opt.get_cost_basis()

    stat.cash += cash_change
    stat.total_commission += opt_comm * abs(opt.num_contracts)
    stat.total_capital_gain += capital_gain
    stat.annual_capital_gain += capital_gain
    if opt.opt_type == 'p':
        stat.total_put_capital_gain += capital_gain
    else:
        stat.total_call_capital_gain += capital_gain

    date_str = date.strftime('%Y-%m-%d')
    add_monthly_csv_row(
        param=param, date=date_str,
        action='sell' if is_long else 'buy',
        entType='call' if opt.opt_type == 'c' else 'put',
        legRole=opt.leg_role, reason=reason,
        stkPrice=stock_price,
        contracts=opt.num_contracts,
        optOpenDate=opt.open_date.strftime('%Y-%m-%d') if opt.open_date is not None else '',
        optOpenPrice=opt.open_price,
        optPrice=trade_px,
        strike=opt.strike,
        expDate=opt.exp_date.strftime('%Y-%m-%d') if opt.exp_date is not None else '',
        optGain=capital_gain,
        structurePnL=stat.structure_pnl(group) if stat.opt_list else 0,
        stat=stat, price_group=group,
    )
    stat.trades.append({
        'open_date': opt.open_date,
        'close_date': date,
        'leg_role': opt.leg_role,
        'opt_type': opt.opt_type,
        'strike': opt.strike,
        'contracts': opt.num_contracts,
        'open_price': opt.open_price,
        'close_price': trade_px,
        'pnl': capital_gain,
        'reason': reason,
    })
    _log(debug, f'{date_str}: close {opt} px={trade_px:.3f} gain={capital_gain:.0f} ({reason})')
    return capital_gain


def close_all_options(param, date, group, stat, reason, debug=False):
    """Close every option leg."""
    total = 0.0
    for opt in list(stat.opt_list):
        total += _close_one_option(param, date, group, stat, opt, reason, debug=debug)
    stat.opt_list = []
    stat.invalidate_balance_cache()
    return total


def close_all_stock(param, date, group, stat, reason, debug=False):
    """Flatten hedge stock (gamma-scalp inventory)."""
    shares = stat.total_shares()
    if abs(shares) < 1e-9:
        return 0.0
    spot = float(group['UnderlyingPrice'].iloc[0])
    stock_comm = get_stock_commission_per_share(param, group)
    if shares > 0:
        close_px = spot - stock_comm  # sell
        gain, avg = stat.close_stock_positions(shares, close_px)
        stat.cash += close_px * shares
        action_shares = -shares
    else:
        # cover short
        close_px = spot + stock_comm  # buy
        # close_stock_positions expects positive num; handle shorts manually
        gain = 0.0
        remaining = -shares
        for pos in list(stat.stocks):
            if remaining <= 0:
                break
            if pos.num_shares >= 0:
                continue
            take = min(-pos.num_shares, remaining)
            gain += (pos.open_price - close_px) * take  # short P/L
            pos.num_shares += take
            remaining -= take
            if abs(pos.num_shares) < 1e-9:
                stat.stocks.remove(pos)
        stat.total_capital_gain += gain
        stat.total_stock_capital_gain += gain
        stat.annual_capital_gain += gain
        stat.cash -= close_px * (-shares)
        action_shares = -shares  # buying abs(shares)

    date_str = date.strftime('%Y-%m-%d')
    add_monthly_csv_row(
        param=param, date=date_str, action='sell' if shares > 0 else 'buy',
        entType='stock', legRole='hedge', reason=reason,
        stkShares=action_shares, stkOpenPrice=0, stkPrice=spot,
        stkGain=gain, stat=stat, price_group=group,
    )
    _log(debug, f'{date_str}: flatten stock shares={shares:.0f} gain={gain:.0f}')
    stat.invalidate_balance_cache()
    return gain


def should_roll(param, date, group, stat):
    """Return (True, reason) if close-and-roll rules fire."""
    if not stat.opt_list:
        return False, ''

    short_dte = stat.short_min_dte(date)
    if param.enable_dte_roll and short_dte is not None:
        if short_dte <= param.roll_dte:
            return True, 'dte_roll'
        if param.roll_on_expiry and short_dte <= 0:
            return True, 'expire'

    pnl_pct = stat.structure_pnl_pct(group)
    if param.enable_profit_take and pnl_pct >= param.profit_take_pct:
        return True, 'profit_take'
    if param.enable_loss_cut and pnl_pct <= -param.loss_cut_pct:
        return True, 'loss_cut'
    return False, ''


def gamma_scalp_hedge(param, date, group, stat, debug=False):
    """If theta < 0 (gamma useful), hedge when |net delta| exceeds threshold.

    Net delta includes stock. Target delta = 0 by trading shares.
    Delta threshold is expressed as fraction of notional short risk:
    |net_delta| / (100 * units * max(n_call, n_put)) > thresh.
    """
    if not param.enable_gamma_scalp or not stat.opt_list:
        return

    update_greeks(param, date, float(group['UnderlyingPrice'].iloc[0]), stat)
    net_delta, net_gamma, net_theta = stat.net_greeks()

    # Start scalping only when book theta is negative (long-gamma regime)
    if net_theta >= 0 and not stat.gamma_scalping_active:
        return

    scale = CONTRACT_SCALE * param.units * max(param.n_call, param.n_put, 1)
    delta_frac = abs(net_delta) / scale if scale else abs(net_delta)

    if delta_frac < param.gamma_scalp_delta_thresh and not stat.gamma_scalping_active:
        return
    if delta_frac < param.gamma_scalp_stop_delta and stat.gamma_scalping_active:
        # Optionally leave residual hedge; still rebalance if way off
        if delta_frac < param.gamma_scalp_stop_delta:
            return

    # Hedge to delta 0: buy -net_delta shares (if net_delta > 0, sell shares)
    shares_needed = -net_delta
    if abs(shares_needed) < 1:
        return

    spot = float(group['UnderlyingPrice'].iloc[0])
    stock_comm = get_stock_commission_per_share(param, group)
    date_str = date.strftime('%Y-%m-%d')

    if shares_needed > 0:
        open_px = spot + stock_comm
        stat.open_stock(shares_needed, open_px, date)
        stat.cash -= open_px * shares_needed
        stat.total_commission += stock_comm * shares_needed
        action = 'buy'
    else:
        # sell / short
        n = -shares_needed
        open_px = spot - stock_comm
        # If we have long shares, sell them first
        long_shares = sum(p.num_shares for p in stat.stocks if p.num_shares > 0)
        sell_from_long = min(long_shares, n)
        gain = 0.0
        if sell_from_long > 0:
            gain, _ = stat.close_stock_positions(sell_from_long, open_px)
            stat.cash += open_px * sell_from_long
        remaining = n - sell_from_long
        if remaining > 0:
            stat.open_stock(-remaining, open_px, date)  # short
            stat.cash += open_px * remaining
        stat.total_commission += stock_comm * n
        action = 'sell'
        shares_needed = -n  # for log

    stat.gamma_scalping_active = True
    stat.num_gamma_hedges += 1
    stat.invalidate_balance_cache()
    nd, ng, nt = stat.net_greeks()
    add_monthly_csv_row(
        param=param, date=date_str, action=action, entType='stock',
        legRole='hedge', reason='gamma_scalp',
        stkShares=shares_needed if action == 'buy' else -abs(shares_needed),
        stkPrice=spot,
        netDelta=nd, netGamma=ng, netTheta=nt,
        structurePnL=stat.structure_pnl(group),
        structurePnLPct=stat.structure_pnl_pct(group),
        stat=stat, price_group=group,
    )
    _log(debug, f'{date_str}: gamma hedge Δwas={net_delta:.1f} θ={net_theta:.2f} '
         f'shares={shares_needed:.0f}')


def maybe_mark_expected_pnl(param, date, group, stat, spots_history, debug=False):
    if not param.eval_expected_pnl or not stat.opt_list:
        return
    spot = float(group['UnderlyingPrice'].iloc[0])
    exp, pnls, probs = expected_structure_pnl(
        stat.opt_list, spot, date, stat.structure_entry_cash,
        param.expected_pnl_move_pcts,
        spots_history=spots_history,
        horizon_days=max(param.roll_dte, 5),
        use_empirical=len(spots_history) > 40,
    )
    stat.last_expected_pnl = exp
    date_str = date.strftime('%Y-%m-%d')
    nd, ng, nt = stat.net_greeks()
    add_monthly_csv_row(
        param=param, date=date_str, action='mark', entType='structure',
        reason='expected_pnl', stkPrice=spot,
        structurePnL=stat.structure_pnl(group),
        structurePnLPct=stat.structure_pnl_pct(group),
        netDelta=nd, netGamma=ng, netTheta=nt,
        expectedPnL=exp,
        stat=stat, price_group=group,
    )
    _log(debug, f'{date_str}: E[PnL]={exp:.0f} structurePnL={stat.structure_pnl(group):.0f}')


def w_strategy(df, param, debug=False, PositionsAndStats=None):
    """Run the W strategy over a daily options DataFrame grouped by DataDate."""
    assert PositionsAndStats is not None
    stat = PositionsAndStats()
    # Work on a compact frame; normalize once for fast expiry joins
    work = df
    if 'ExpirationDate' in work.columns:
        if not pd.api.types.is_datetime64_any_dtype(work['ExpirationDate']):
            work = work.copy()
            work['ExpirationDate'] = pd.to_datetime(work['ExpirationDate'])
        # Ensure normalized dates without rewriting huge frames repeatedly
        if getattr(work, '_w_norm_exp', None) is None:
            work = work.copy()
            work['ExpirationDate'] = work['ExpirationDate'].dt.normalize()
            work['DataDate'] = work['DataDate'].dt.normalize()
            work._w_norm_exp = True
    stat.all_data = work
    stat.cash = param.fixed_param.initial_balance

    grouped = {d: g for d, g in work.groupby('DataDate', sort=True)}
    dates = list(grouped.keys())
    if not dates:
        return {'error': 'no data'}

    spots_history = []
    start_date = pd.Timestamp(dates[0])
    end_date = pd.Timestamp(dates[-1])
    prev_year = start_date.year

    for i, raw_date in enumerate(dates):
        group = grouped[raw_date]
        date = pd.Timestamp(raw_date).normalize()
        stock_price = float(group['UnderlyingPrice'].iloc[0])
        spots_history.append(stock_price)
        date_str = date.strftime('%Y-%m-%d')

        if i == 0:
            stat.initial_price = stock_price
            stat.cur_peak = stat.cash
            stat.cur_peak_date = date_str

        # Year-end tax on calendar rollover
        if date.year != prev_year:
            year_end_tax(param, date - pd.Timedelta(days=1), stat, debug=debug)
            prev_year = date.year

        # Greeks only when needed for gamma-scalp decisions
        if param.enable_gamma_scalp and stat.opt_list:
            update_greeks(param, date, stock_price, stat)

        # Close/roll rules
        if stat.opt_list:
            do_roll, reason = should_roll(param, date, group, stat)
            if do_roll:
                close_all_options(param, date, group, stat, reason, debug=debug)
                close_all_stock(param, date, group, stat, reason, debug=debug)
                stat.num_rolls += 1
                open_w_structure(param, date, group, stat, debug=debug)
            else:
                gamma_scalp_hedge(param, date, group, stat, debug=debug)
        else:
            open_w_structure(param, date, group, stat, debug=debug)

        # Periodic expected P/L (weekly-ish: every 5 sessions)
        if param.eval_expected_pnl and i % 5 == 0:
            if param.enable_gamma_scalp:
                pass  # already updated
            else:
                update_greeks(param, date, stock_price, stat)
            maybe_mark_expected_pnl(
                param, date, group, stat, spots_history, debug=debug)

        nlv, _ = stat.get_balance(group)
        calc_drawdown(stat, date_str, nlv + stat.cash)

    # Final close
    last_group = grouped[dates[-1]]
    last_date = pd.Timestamp(dates[-1]).normalize()
    if stat.opt_list:
        close_all_options(param, last_date, last_group, stat, 'final', debug=debug)
    if abs(stat.total_shares()) > 1e-9:
        close_all_stock(param, last_date, last_group, stat, 'final', debug=debug)
    year_end_tax(param, last_date, stat, debug=debug)

    years = max((end_date - start_date).days / 365.25, 1e-6)
    final_nlv = stat.cash
    total_return = final_nlv / param.fixed_param.initial_balance
    annual_return = (total_return ** (1 / years) - 1) * 100 if total_return > 0 else -100

    tax_rate = (stat.total_tax / final_nlv * 100) if final_nlv else 0
    comm_rate = (abs(stat.total_commission) / final_nlv * 100) if final_nlv else 0

    add_summary_csv_row(
        param=param,
        annualReturn=annual_return,
        maxDrawdown=stat.max_drawdown,
        taxRate=tax_rate,
        commRate=comm_rate,
        totalOptGain=stat.total_put_capital_gain + stat.total_call_capital_gain,
        totalStkGain=stat.total_stock_capital_gain,
        expectedPnL=stat.last_expected_pnl,
        numRolls=stat.num_rolls,
        numGammaHedges=stat.num_gamma_hedges,
    )

    return {
        'annualReturn': annual_return,
        'maxDrawdown': stat.max_drawdown,
        'finalNLV': final_nlv,
        'numRolls': stat.num_rolls,
        'numGammaHedges': stat.num_gamma_hedges,
        'expectedPnL': stat.last_expected_pnl,
        'stat': stat,
    }
