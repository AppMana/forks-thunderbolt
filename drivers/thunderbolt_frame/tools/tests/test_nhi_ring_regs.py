#!/usr/bin/env python3
"""Unit tests for NHI producer/consumer ring arithmetic."""

import importlib.util
import pathlib
import unittest


SCRIPT = pathlib.Path(__file__).parents[1] / "nhi-ring-regs.py"
SPEC = importlib.util.spec_from_file_location("nhi_ring_regs", SCRIPT)
NHI = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(NHI)


class RingArithmeticTest(unittest.TestCase):
    def test_tx_wrap_uses_configured_ring_size(self):
        self.assertEqual(NHI.ring_distance(8, 9, 10), 9)

    def test_equal_indexes_are_empty(self):
        self.assertEqual(NHI.ring_distance(209, 209, 10), 0)

    def test_rx_armed_wrap_uses_configured_ring_size(self):
        self.assertEqual(NHI.ring_distance(208, 209, 2048), 2047)

    def test_invalid_ring_size_is_rejected(self):
        with self.assertRaises(ValueError):
            NHI.ring_distance(0, 0, 0)


if __name__ == "__main__":
    unittest.main()
