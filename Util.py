#!/usr/bin/env python3
"""General utility functions."""

from datetime import datetime, timedelta


def assert_close(a, b):
    """Assert that two values are close to each other (within 0.1%)."""
    if abs(a) < 0.0001 and abs(b) < 0.0001:
        return
    assert abs(a - b) / a < 0.001


def round_float(v, digits=-1):
    """Round a float value for display purposes.

    Args:
        v: Value to round
        digits: Number of decimal digits. -1 means auto-determine based on magnitude.

    Returns:
        Rounded value or original value if not a float.
    """
    if not isinstance(v, float):
        return v
    if digits == -1:
        if v > 1000:
            return f'{int(v + 0.5):,}'
        elif v > 100:
            return int(v + 0.5)
        elif v > 10:  # round to 1 digits
            return int(v * 10 + 0.5) / 10
        else:  # round to 2 digits
            return int(v * 100 + 0.5) / 100
    else:
        n = 10**digits
        return int(v * n) / n


def calculate_quartiles(data):
    """Calculate quartile boundaries for a list of values.

    Args:
        data: List of numeric values

    Returns:
        Tuple of (q1, q2, q3) representing 25th, 50th, and 75th percentiles.
    """
    sorted_values = sorted(data)
    n = len(sorted_values)
    q1 = sorted_values[n // 4]
    q2 = sorted_values[n // 2]
    q3 = sorted_values[(3 * n) // 4]
    return q1, q2, q3


# ============================================================================
# Date Helper Functions
# ============================================================================

def third_friday(year, month):
    """Return the date of the third Friday of a given month."""
    first_day_of_month = datetime(year, month, 1)
    third_fri = first_day_of_month + \
        timedelta(days=((4 - first_day_of_month.weekday()) % 7) + 14)
    return third_fri


def third_fri_after_n_mon(year, month, n):
    """Third Friday of the calendar month that is n months after (year, month)."""
    if n < 1:
        raise ValueError(f'n must be >= 1, got {n}')
    total_m = month + n
    y = year + (total_m - 1) // 12
    m = ((total_m - 1) % 12) + 1
    return third_friday(y, m)


def third_fri_after_2_mon(year, month):
    """Return the date of the third Friday two months later."""
    return third_fri_after_n_mon(year, month, 2)
