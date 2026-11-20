import struct
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import dd_journal as journal


class History(journal.Journal):
    """Mock the OS read operation, including reading past a pre-sampled checkpoint."""
    def __init__(self, events=()):
        self.events = list(events)

    def request(self, code, query, size=65536):
        start, = struct.unpack_from("<q", query)
        rows = []
        for usn, file_id, parent, reason in self.events:
            if usn < start:
                continue
            record = bytearray(64)
            struct.pack_into("<IHHQQq", record, 0, len(record), 2, 0, file_id, parent, usn)
            struct.pack_into("<I", record, 40, reason)
            rows.append(record)
        return struct.pack("<q", max(start + 64, 128 * 1024)) + b"".join(rows)


class JournalTests(unittest.TestCase):
    def test_open_writer_retains_cursor_until_close(self):
        api = History([(100, 7, 4, 2), (200, 7, 4, 3)])
        cp = ("42", 300, 80)
        cursor = api.baseline_cursor(cp)
        self.assertEqual(cursor, 100)
        # Further writes with the same reason can happen without a new USN record.
        for end in (300, 400):
            changes, cursor = api.changes("42", cursor, ("42", end, 80))
            self.assertEqual(changes, ({4}, {7}))
            self.assertEqual(cursor, 100)
        api.events.append((400, 7, 4, journal.CLOSE | 3))
        changes, cursor = api.changes("42", cursor, ("42", 500, 80))
        self.assertEqual(changes, ({4}, {7}))
        self.assertEqual(cursor, 500)
        self.assertEqual(api.changes("42", cursor, ("42", 500, 80)), ((set(), set()), 500))

    def test_only_retained_open_handles_pin_baseline(self):
        api = History([(100, 6, 1, 2), (200, 7, 2, 4), (300, 6, 1, journal.CLOSE | 2),
                       (350, 6, 1, 1), (400, 7, 2, journal.CLOSE | 4)])
        self.assertEqual(api.baseline_cursor(("7", 400, 0)), 200)
        self.assertEqual(api.baseline_cursor(("7", 450, 0)), 350)
        # A close beyond the sampled end must not advance the cursor early.
        self.assertEqual(api.changes("7", 200, ("7", 400, 0))[1], 200)

    def test_ignored_cache_traffic_does_not_invalidate_other_files(self):
        api = History([(10, 20, 100, 0x800), (20, 21, 100, 2), (30, 22, 101, journal.CLOSE | 1)])
        ignore = lambda file_id, parent_id: parent_id == 100
        self.assertEqual(api.baseline_cursor(("42", 40, 0), ignore), 40)
        self.assertEqual(api.changes("42", 0, ("42", 40, 0), ignore), (({101}, {22}), 40))
        with self.assertRaises(ValueError):
            api.changes("42", 0, ("42", 40, 0))

    def test_unavailable_and_unbounded_histories_require_traversal(self):
        api = History([(10, 1, 100, 4), (20, 2, 100, 2)])
        for limit in ("MAX_OPEN_WRITERS", "MAX_JOURNAL_BYTES"):
            with self.subTest(limit=limit), patch.object(journal, limit, 1):
                self.assertIsNone(api.baseline_cursor(("42", 50, 0)))
                with self.assertRaises(ValueError):
                    api.changes("42", 0, ("42", 50, 0))
        with patch.object(api, "request", side_effect=OSError("journal reset")):
            self.assertIsNone(api.baseline_cursor(("42", 50, 0)))


if __name__ == "__main__":
    unittest.main()
