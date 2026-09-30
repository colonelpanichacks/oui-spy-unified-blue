#!/usr/bin/env python3
"""Unit tests for battery SOC plausibility filter (mirrors expander.cpp logic)."""
from __future__ import annotations

import unittest

K_SOC_DOUBLE_READ_MAX_DELTA = 8
K_VOLTAGE_SOC_MAX_DELTA = 25
K_MAX_SOC_DELTA_PER_UPDATE = 10
K_STABLE_VOLTAGE_DELTA_V = 0.05
K_HIGH_VOLTAGE_THRESHOLD_V = 3.85
K_LOW_SOC_AT_HIGH_VOLTAGE = 30
K_EMA_SMALL_CHANGE_THRESHOLD = 3

BATTERY_ACCEPT = 0
BATTERY_REJECT_DOUBLE_READ = 1
BATTERY_REJECT_VOLTAGE_SOC = 2
BATTERY_REJECT_MAX_DELTA = 3


def soc_from_voltage(volts: float) -> int:
    curve = [
        (2.50, 0),
        (3.30, 0),
        (3.40, 5),
        (3.60, 20),
        (3.70, 45),
        (3.80, 50),
        (4.00, 85),
        (4.20, 100),
    ]
    if volts <= curve[0][0]:
        return 0
    if volts >= curve[-1][0]:
        return 100
    for i in range(len(curve) - 1):
        v0, p0 = curve[i]
        v1, p1 = curve[i + 1]
        if volts <= v1:
            span = v1 - v0
            if span <= 0:
                return p1
            t = (volts - v0) / span
            return int(p0 + t * (p1 - p0) + 0.5)
    return 50


def abs_delta_u8(a: int, b: int) -> int:
    return abs(a - b)


def check_plausibility(
    volts: float,
    soc: int,
    soc2: int,
    filtered_percent: int,
    last_accepted_voltage: float,
    charging: bool,
) -> int:
    if abs_delta_u8(soc, soc2) > K_SOC_DOUBLE_READ_MAX_DELTA:
        return BATTERY_REJECT_DOUBLE_READ

    ocv_estimate = soc_from_voltage(volts)
    if volts > K_HIGH_VOLTAGE_THRESHOLD_V and soc < K_LOW_SOC_AT_HIGH_VOLTAGE:
        return BATTERY_REJECT_VOLTAGE_SOC
    if (
        abs_delta_u8(soc, ocv_estimate) > K_VOLTAGE_SOC_MAX_DELTA
        and abs(volts - last_accepted_voltage) < K_STABLE_VOLTAGE_DELTA_V
    ):
        return BATTERY_REJECT_VOLTAGE_SOC

    soc_delta = soc - filtered_percent
    if (
        abs(soc_delta) > K_MAX_SOC_DELTA_PER_UPDATE
        and abs(volts - last_accepted_voltage) < K_STABLE_VOLTAGE_DELTA_V
    ):
        if not (charging and soc_delta > 0):
            return BATTERY_REJECT_MAX_DELTA
    return BATTERY_ACCEPT


def apply_ema(filtered: int, raw_soc: int) -> int:
    delta = abs(raw_soc - filtered)
    if delta <= K_EMA_SMALL_CHANGE_THRESHOLD:
        return (filtered * 4 + raw_soc + 2) // 5
    return raw_soc


def filter_reading(
    volts: float,
    soc: int,
    soc2: int,
    filtered_percent: int,
    last_accepted_voltage: float,
    charging: bool,
    has_prior: bool,
) -> tuple[int, float, bool]:
    if not has_prior:
        return soc, volts, True

    reason = check_plausibility(
        volts, soc, soc2, filtered_percent, last_accepted_voltage, charging
    )
    if reason != BATTERY_ACCEPT:
        return filtered_percent, last_accepted_voltage, True

    new_pct = apply_ema(filtered_percent, soc)
    return new_pct, volts, True


class BatteryFilterTests(unittest.TestCase):
    def test_high_voltage_low_soc_rejected(self) -> None:
        pct, volts, _ = filter_reading(
            4.00, 6, 6, 90, 4.00, False, has_prior=True
        )
        self.assertEqual(pct, 90)
        self.assertAlmostEqual(volts, 4.00)

    def test_consistent_high_reading_accepted(self) -> None:
        pct, volts, _ = filter_reading(
            3.95, 88, 88, 90, 3.95, False, has_prior=True
        )
        self.assertEqual(pct, 90)  # EMA(90, 88) with 2-point delta
        self.assertAlmostEqual(volts, 3.95)

    def test_real_discharge_with_voltage_drop_accepted(self) -> None:
        pct, volts, _ = filter_reading(
            3.55, 15, 15, 90, 4.00, False, has_prior=True
        )
        self.assertEqual(pct, 15)
        self.assertAlmostEqual(volts, 3.55)

    def test_impossible_drop_with_stable_voltage_rejected(self) -> None:
        pct, _, _ = filter_reading(
            3.70, 45, 45, 90, 3.71, False, has_prior=True
        )
        self.assertEqual(pct, 90)

    def test_charging_upward_jump_allowed(self) -> None:
        pct, volts, _ = filter_reading(
            3.95, 65, 65, 50, 3.95, True, has_prior=True
        )
        self.assertEqual(pct, 65)
        self.assertAlmostEqual(volts, 3.95)

    def test_double_read_mismatch_rejected(self) -> None:
        reason = check_plausibility(4.00, 90, 6, 90, 4.00, False)
        self.assertEqual(reason, BATTERY_REJECT_DOUBLE_READ)

    def test_soc_from_voltage_mid_band(self) -> None:
        self.assertEqual(soc_from_voltage(3.70), 45)
        self.assertGreaterEqual(soc_from_voltage(4.00), 80)

    def test_ema_small_change(self) -> None:
        self.assertEqual(apply_ema(90, 87), 89)


if __name__ == "__main__":
    unittest.main()
