#!/usr/bin/env python3
"""Parameter classes for the W tail-hedge strategy."""


class FixedParam:
    """Fixed parameters that don't change during optimization."""

    def __init__(self,
                 initial_balance=1e6,
                 capital_gain_tax=0.371,
                 stock_comm_per_share=0.0035,
                 opt_comm_per_contract=0.065,
                 stock_slippage=0,
                 opt_slippage_percent=1,
                 monthly_writer=None,
                 summary_writer=None,
                 symbol='SPY') -> None:
        self.initial_balance = initial_balance
        self.capital_gain_tax = capital_gain_tax
        self.stock_comm_per_share = stock_comm_per_share
        self.opt_comm_per_contract = opt_comm_per_contract
        self.stock_slippage = stock_slippage
        self.opt_slippage_percent = opt_slippage_percent / 100
        self.monthly_writer = monthly_writer
        self.summary_writer = summary_writer
        self.symbol = symbol


class WParam:
    """Parameters for one W-strategy configuration.

    Short legs collect premium near ATM; long legs hedge tails with smaller
    absolute delta. M_* must be >= N_* so the wings can out-earn short losses
    in a large move. Prefer total long |theta| < total short |theta| so the
    book does not bleed when the market is quiet.
    """

    def __init__(self,
                 fixed_param=None,
                 # Tenors (calendar days to expiration when opened)
                 short_dte=30,
                 long_dte=60,
                 # Absolute deltas used to pick strikes (|delta|)
                 short_delta=0.40,
                 long_delta=0.10,
                 # Contract counts per W unit (shorts N, longs M, M >= N)
                 n_call=1,
                 n_put=1,
                 m_call=2,
                 m_put=2,
                 units=1,
                 # Close / roll rules
                 roll_dte=7,                 # roll when short DTE <= this
                 roll_on_expiry=True,        # also roll when short expired
                 profit_take_pct=50,         # close if structure P/L >= this % of |entry debit/credit|
                 loss_cut_pct=100,           # close if structure P/L <= -this % of |entry|
                 enable_profit_take=True,
                 enable_loss_cut=True,
                 enable_dte_roll=True,
                 # Dynamic gamma scalping
                 enable_gamma_scalp=True,
                 gamma_scalp_delta_thresh=0.15,  # hedge when |net delta|/shares_equiv beyond this
                 gamma_scalp_stop_delta=0.05,    # optional: do not re-hedge below this
                 # Expected P/L evaluation
                 eval_expected_pnl=True,
                 expected_pnl_move_pcts=None,  # e.g. [-0.3,-0.2,...,0.3]
                 ) -> None:
        self.fixed_param = fixed_param
        self.short_dte = int(short_dte)
        self.long_dte = int(long_dte)
        self.short_delta = float(short_delta)
        self.long_delta = float(long_delta)
        self.n_call = int(n_call)
        self.n_put = int(n_put)
        self.m_call = int(m_call)
        self.m_put = int(m_put)
        self.units = int(units)
        self.roll_dte = int(roll_dte)
        self.roll_on_expiry = bool(roll_on_expiry)
        self.profit_take_pct = float(profit_take_pct) / 100.0
        self.loss_cut_pct = float(loss_cut_pct) / 100.0
        self.enable_profit_take = bool(enable_profit_take)
        self.enable_loss_cut = bool(enable_loss_cut)
        self.enable_dte_roll = bool(enable_dte_roll)
        self.enable_gamma_scalp = bool(enable_gamma_scalp)
        self.gamma_scalp_delta_thresh = float(gamma_scalp_delta_thresh)
        self.gamma_scalp_stop_delta = float(gamma_scalp_stop_delta)
        self.eval_expected_pnl = bool(eval_expected_pnl)
        if expected_pnl_move_pcts is None:
            self.expected_pnl_move_pcts = [
                -0.30, -0.20, -0.15, -0.10, -0.05, 0.0,
                0.05, 0.10, 0.15, 0.20, 0.30
            ]
        else:
            self.expected_pnl_move_pcts = list(expected_pnl_move_pcts)

        if self.m_call < self.n_call or self.m_put < self.n_put:
            raise ValueError(
                f'Long contracts must be >= short: '
                f'M_call={self.m_call} N_call={self.n_call} '
                f'M_put={self.m_put} N_put={self.n_put}')
        if self.long_dte < self.short_dte:
            raise ValueError(
                f'long_dte ({self.long_dte}) must be >= short_dte ({self.short_dte})')
        if not (0 < self.long_delta < self.short_delta < 1):
            raise ValueError(
                f'Need 0 < long_delta ({self.long_delta}) < '
                f'short_delta ({self.short_delta}) < 1')

    def label(self) -> str:
        return (f'sDTE={self.short_dte} lDTE={self.long_dte} '
                f'sΔ={self.short_delta:.2f} lΔ={self.long_delta:.2f} '
                f'N={self.n_call}/{self.n_put} M={self.m_call}/{self.m_put} '
                f'roll={self.roll_dte} γscalp={int(self.enable_gamma_scalp)}'
                f'@{self.gamma_scalp_delta_thresh:.2f}')


def parse_float_range(spec, as_int=False):
    """Parse 'v' or 'start,stop,step' into a list of values.

    Stop is inclusive when using start,stop,step.
    """
    parts = [p.strip() for p in str(spec).split(',') if p.strip() != '']
    if len(parts) == 1:
        v = float(parts[0])
        return [int(v) if as_int else v]
    if len(parts) != 3:
        raise ValueError(f'Expected v or start,stop,step; got {spec!r}')
    start, stop, step = (float(parts[0]), float(parts[1]), float(parts[2]))
    if step == 0:
        raise ValueError(f'step cannot be 0 in {spec!r}')
    out = []
    x = start
    # Inclusive stop with float-safe loop
    if step > 0:
        while x <= stop + abs(step) * 1e-9:
            out.append(int(round(x)) if as_int else x)
            x += step
    else:
        while x >= stop - abs(step) * 1e-9:
            out.append(int(round(x)) if as_int else x)
            x += step
    return out


def expand_w_params(fixed_param, args_dict):
    """Cartesian product of ranged W parameters from CLI-style dict."""
    short_dtes = parse_float_range(args_dict['short_dte'], as_int=True)
    long_dtes = parse_float_range(args_dict['long_dte'], as_int=True)
    short_deltas = parse_float_range(args_dict['short_delta'])
    long_deltas = parse_float_range(args_dict['long_delta'])
    roll_dtes = parse_float_range(args_dict['roll_dte'], as_int=True)
    gamma_threshs = parse_float_range(args_dict['gamma_scalp_delta_thresh'])

    params = []
    for sd in short_dtes:
        for ld in long_dtes:
            if ld < sd:
                continue
            for sdelta in short_deltas:
                for ldelta in long_deltas:
                    if not (0 < ldelta < sdelta < 1):
                        continue
                    for rd in roll_dtes:
                        for gth in gamma_threshs:
                            params.append(WParam(
                                fixed_param=fixed_param,
                                short_dte=sd,
                                long_dte=ld,
                                short_delta=sdelta,
                                long_delta=ldelta,
                                n_call=args_dict['n_call'],
                                n_put=args_dict['n_put'],
                                m_call=args_dict['m_call'],
                                m_put=args_dict['m_put'],
                                units=args_dict['units'],
                                roll_dte=rd,
                                roll_on_expiry=args_dict['roll_on_expiry'],
                                profit_take_pct=args_dict['profit_take_pct'],
                                loss_cut_pct=args_dict['loss_cut_pct'],
                                enable_profit_take=args_dict['enable_profit_take'],
                                enable_loss_cut=args_dict['enable_loss_cut'],
                                enable_dte_roll=args_dict['enable_dte_roll'],
                                enable_gamma_scalp=args_dict['enable_gamma_scalp'],
                                gamma_scalp_delta_thresh=gth,
                                gamma_scalp_stop_delta=args_dict['gamma_scalp_stop_delta'],
                                eval_expected_pnl=args_dict['eval_expected_pnl'],
                            ))
    return params
