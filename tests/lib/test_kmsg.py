# SPDX-License-Identifier: GPL-2.0
"""Unit tests for kmsg.py (no kernel needed): python3 -m unittest discover -s tests/lib"""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kmsg  # noqa: E402


def rec(text, level=6, seq=0):
    return kmsg.Record(level=level, seq=seq, usec=0, text=text)


class ParseRecord(unittest.TestCase):
    def test_basic(self):
        r = kmsg.parse_record("6,1234,5678901,-;lkl_hello: loaded\n")
        self.assertEqual(r, kmsg.Record(6, 1234, 5678901, "lkl_hello: loaded"))

    def test_level_is_low_three_bits_of_priority(self):
        # facility user (1) << 3 | warning (4) = 12
        self.assertEqual(kmsg.parse_record("12,1,2,-;x").level, 4)

    def test_extra_header_fields_and_dictionary_lines(self):
        raw = "3,7,99,c,caller=T42;msg here\n SUBSYSTEM=pci\n DEVICE=+pci:0000\n"
        r = kmsg.parse_record(raw)
        self.assertEqual((r.level, r.text), (3, "msg here"))

    def test_hex_escapes_are_decoded(self):
        self.assertEqual(kmsg.parse_record("6,1,1,-;a\\x09b").text, "a\tb")

    def test_malformed(self):
        for raw in ("", "no separator", "x,y,z,-;text", "6,1;text"):
            self.assertIsNone(kmsg.parse_record(raw), raw)


class Marker(unittest.TestCase):
    def test_after_last_marker(self):
        recs = [rec("old"), rec("M"), rec("a"), rec("M"), rec("b"), rec("c")]
        self.assertEqual([r.text for r in kmsg.after_marker(recs, "M")], ["b", "c"])

    def test_missing_marker(self):
        with self.assertRaises(LookupError):
            kmsg.after_marker([rec("x")], "M")


class Expect(unittest.TestCase):
    recs = [rec(t) for t in ["step 1", "step 2", "undo 2", "undo 1", "done"]]

    def test_in_order(self):
        ok, _ = kmsg.expect_in_order(self.recs, ["step 1", "undo 2", "undo 1"])
        self.assertTrue(ok)

    def test_wrong_order_fails(self):
        ok, detail = kmsg.expect_in_order(self.recs, ["undo 1", "undo 2"])
        self.assertFalse(ok)
        self.assertIn("undo 2", detail)

    def test_same_line_cannot_match_twice(self):
        ok, _ = kmsg.expect_in_order([rec("step 1")], ["step", "step"])
        self.assertFalse(ok)

    def test_regex(self):
        ok, _ = kmsg.expect_in_order(self.recs, [r"step \d", r"^done$"])
        self.assertTrue(ok)


class Problems(unittest.TestCase):
    def test_detects_kernel_problems(self):
        recs = [rec("fine"), rec("WARNING: CPU: 1 PID: 2 at foo.c:3"),
                rec("BUG: KASAN: use-after-free in bar"), rec("ok")]
        self.assertEqual(len(kmsg.find_problems(recs)), 2)

    def test_module_warnings_are_not_problems(self):
        # pr_warn text from our modules must not trip the detector.
        recs = [rec("lkl_params: threshold 0 rejected: must be 1..100", level=4)]
        self.assertEqual(kmsg.find_problems(recs), [])


class Levels(unittest.TestCase):
    def test_names_and_numbers(self):
        self.assertEqual(kmsg.parse_level("warning"), 4)
        self.assertEqual(kmsg.parse_level("ERR"), 3)
        self.assertEqual(kmsg.parse_level("6"), 6)
        with self.assertRaises(ValueError):
            kmsg.parse_level("loud")

    def test_first_match_and_count(self):
        recs = [rec("a", 3), rec("b", 4), rec("a", 6)]
        self.assertEqual(kmsg.first_match(recs, "a").level, 3)
        self.assertEqual(kmsg.count_matches(recs, "a"), 2)
        self.assertIsNone(kmsg.first_match(recs, "z"))


class ReadFromFile(unittest.TestCase):
    """read_kmsg() reads one record per read(); a regular file returns
    everything at once, so exercise the single-record path explicitly."""

    def test_reads_records(self):
        with tempfile.NamedTemporaryFile("w", delete=False) as f:
            f.write("6,1,1,-;only record\n")
            path = f.name
        try:
            recs = kmsg.read_kmsg(path)
            self.assertEqual([r.text for r in recs], ["only record"])
        finally:
            os.unlink(path)


if __name__ == "__main__":
    unittest.main()
