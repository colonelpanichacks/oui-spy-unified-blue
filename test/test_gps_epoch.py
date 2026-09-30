#!/usr/bin/env python3
"""Host-side check for UTC epoch ms conversion (mirrors board_gps.cpp)."""

from __future__ import annotations


def is_leap_year(year: int) -> bool:
    return (year % 4 == 0 and year % 100 != 0) or (year % 400 == 0)


def utc_epoch_ms_from_components(
    year: int,
    month: int,
    day: int,
    hour: int,
    minute: int,
    second: int,
    centisecond: int,
) -> int:
    if (
        year < 2000
        or year > 2100
        or month < 1
        or month > 12
        or day < 1
        or day > 31
        or hour < 0
        or hour > 23
        or minute < 0
        or minute > 59
        or second < 0
        or second > 60
        or centisecond < 0
        or centisecond > 99
    ):
        return 0

    days_in_month = [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
    days = 0
    for y in range(1970, year):
        days += 366 if is_leap_year(y) else 365
    for m in range(1, month):
        days += days_in_month[m - 1]
        if m == 2 and is_leap_year(year):
            days += 1
    days += day - 1

    sec = days * 86400 + hour * 3600 + minute * 60 + second
    if sec < 0:
        return 0
    return sec * 1000 + centisecond * 10


def test_epoch_conversion() -> None:
    # 2026-07-22T22:42:00.00Z
    ms = utc_epoch_ms_from_components(2026, 7, 22, 22, 42, 0, 0)
    assert ms > 1_700_000_000_000, ms
    assert ms == 1_784_760_120_000, ms

    # Unix epoch
    assert utc_epoch_ms_from_components(1970, 1, 1, 0, 0, 0, 0) == 0

    # Invalid input
    assert utc_epoch_ms_from_components(1999, 1, 1, 0, 0, 0, 0) == 0


if __name__ == "__main__":
    test_epoch_conversion()
    print("PASS")
