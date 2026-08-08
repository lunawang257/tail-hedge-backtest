#!/usr/bin/env python3
"""Stock position management."""


class StockPosition:
    """Represents a stock position with cost basis tracking."""

    def __init__(self,
                 open_price=0,
                 open_date=None,
                 num_shares=0) -> None:
        self.open_price = open_price  # includes per-share commission
        self.open_date = open_date
        self.num_shares = num_shares
