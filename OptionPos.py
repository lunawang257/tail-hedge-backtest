#!/usr/bin/env python3
"""Option position management for the W tail-hedge strategy."""

CONTRACT_SCALE = 100

# Leg roles for the W structure
LEG_SHORT_CALL = 'short_call'
LEG_SHORT_PUT = 'short_put'
LEG_LONG_CALL = 'long_call'
LEG_LONG_PUT = 'long_put'
LEG_ROLES = (LEG_SHORT_CALL, LEG_SHORT_PUT, LEG_LONG_CALL, LEG_LONG_PUT)


class OptPosition:
    """Represents an option position (put or call) with Greeks tracking.

    num_contracts > 0 means long; < 0 means short.
    """

    def __init__(self,
                 opt_type='p',  # put or call
                 strike=0,
                 exp_date=None,
                 num_contracts=0,
                 open_date=None,
                 open_price=0,
                 org_price=0,
                 delta=0,
                 gamma=0,
                 theta=0,
                 implied_vol=0,
                 leg_role='',
                 open_stock_price=0,
                 structure_id=0) -> None:
        assert opt_type in ['p', 'c']
        self.opt_type = opt_type
        self.strike = strike
        self.exp_date = exp_date
        self.num_contracts = num_contracts
        self.open_date = open_date
        self.open_price = open_price  # includes slippage & commission (per share)
        self.org_price = org_price  # w/o slippage nor commission
        self.per_contract_delta = delta
        self.per_contract_gamma = gamma
        self.per_contract_theta = theta
        self.implied_vol = implied_vol
        self.leg_role = leg_role
        self.open_stock_price = open_stock_price
        self.structure_id = structure_id

    def total_delta(self) -> float:
        """Total delta for all contracts (signed by long/short)."""
        return self.num_contracts * self.per_contract_delta * CONTRACT_SCALE

    def total_gamma(self) -> float:
        return self.num_contracts * self.per_contract_gamma * CONTRACT_SCALE

    def total_theta(self) -> float:
        """Total daily theta in dollars (approx; Merton theta is per year)."""
        return self.num_contracts * self.per_contract_theta * CONTRACT_SCALE

    def total_max_delta(self) -> float:
        """Max delta if stock drops to 0 (puts become -1)."""
        if self.opt_type == 'p':
            return self.num_contracts * -1 * CONTRACT_SCALE
        return 0.0

    def get_cost_basis(self) -> float:
        """Total cash paid (positive) or received (negative) at open."""
        return self.open_price * self.num_contracts * CONTRACT_SCALE

    def is_short(self) -> bool:
        return self.num_contracts < 0

    def is_long(self) -> bool:
        return self.num_contracts > 0

    def dte(self, as_of) -> int:
        return (pd_timestamp(self.exp_date) - pd_timestamp(as_of)).days

    def __repr__(self):
        date = self.exp_date.strftime("%Y-%m-%d") if self.exp_date is not None else '?'
        role = self.leg_role or self.opt_type
        return (f'{role} {int(self.strike)}{self.opt_type} '
                f'n={self.num_contracts} exp={date}')


def pd_timestamp(d):
    import pandas as pd
    return pd.Timestamp(d).normalize()
