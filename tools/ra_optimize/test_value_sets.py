#!/usr/bin/env python3
"""Unit tests for the symbolic Range Analysis result comparator."""

import unittest
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from evaluate import compare_line


class ValueSetComparisonTest(unittest.TestCase):
    def assert_comparison(
        self, expected: str, actual: str, exact: bool, sound: bool
    ) -> None:
        self.assertEqual(compare_line(expected, actual), (exact, sound))

    def test_enumeration_order_and_duplicates_are_ignored(self) -> None:
        self.assert_comparison("1, 3, 5", "5, 3, 1, 1", True, True)

    def test_interval_and_enumeration_are_semantically_equal(self) -> None:
        self.assert_comparison("1, 3, 5", "[1, 5:2]", True, True)

    def test_large_top_contains_small_set_without_expansion(self) -> None:
        self.assert_comparison(
            "13, 24, 40",
            "[-9223372036854775808, 9223372036854775807:1]",
            False,
            True,
        )

    def test_large_stride_union_can_cover_large_interval(self) -> None:
        self.assert_comparison(
            "[0, 1000000:1]",
            "[0, 1000000:2], [1, 999999:2]",
            True,
            True,
        )

    def test_stride_hole_is_not_sound(self) -> None:
        self.assert_comparison("[0, 1000000:1]", "[0, 1000000:2]", False, False)

    def test_overlapping_intervals_are_compared_as_unions(self) -> None:
        self.assert_comparison(
            "[0, 1000000:1]",
            "[0, 500000:1], [500001, 1000000:1]",
            True,
            True,
        )

    def test_boolean_sets(self) -> None:
        self.assert_comparison("true", "true, false", False, True)
        self.assert_comparison("true, false", "true", False, False)

    def test_actual_subset_is_not_sound(self) -> None:
        self.assert_comparison("[0, 10:1]", "[0, 9:1]", False, False)


if __name__ == "__main__":
    unittest.main()
