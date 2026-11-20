import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

PROJECT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT))
import dd_journal as journal
import worker


def datasets(raw):
    result = []
    stream = io.BytesIO(raw)
    while stream.tell() < len(raw):
        if stream.read(4) != b"DDV1":
            raise AssertionError("Missing dataset")
        marker = stream.read(1)[0]
        entries = []
        while True:
            kind = stream.read(1)
            if kind == b"\x05":
                break
            fields = journal.HEADER.unpack(kind + stream.read(journal.HEADER.size - 1))
            entries.append((*fields[:-1], stream.read(fields[-1])))
        result.append((marker, entries))
    return result


def capture(rows):
    data = io.BytesIO(b"DDV1")
    data.seek(0, 2)
    for *values, name in rows:
        data.write(journal.HEADER.pack(*values, len(name)))
        data.write(name)
    data.write(b"\x05")
    data.seek(0)
    return data


def native(name):
    return name.encode("utf-16-le") if os.name == "nt" else os.fsencode(name)


class WorkerTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.base = Path(tmp.name)
        self.root = self.base / "scan"
        self.root.mkdir()
        self.profile = self.base / "profile"
        env = patch.dict(os.environ, {"LOCALAPPDATA": str(self.profile),
                                     "XDG_CACHE_HOME": str(self.profile)})
        env.start()
        self.addCleanup(env.stop)
        (self.root / "groß.txt").write_bytes(b"abcd")

    def invoke(self, *arguments):
        return subprocess.run([sys.executable, str(PROJECT / "worker.py"), *arguments],
                              capture_output=True, check=True, timeout=20)

    def test_first_scan_preview_update_and_clear(self):
        first, = datasets(self.invoke("scan", str(self.root)).stdout)
        self.assertEqual(first[0], 7)
        self.assertEqual(first[1][1][2], 4)
        self.assertEqual(first[1][1][-1], native("groß.txt"))
        (self.root / "groß.txt").write_bytes(b"another length")
        run = self.invoke("scan", str(self.root))
        saved, new = datasets(run.stdout)
        self.assertIn(b"Saved scan", run.stderr)
        self.assertEqual(saved, (6, first[1]))
        self.assertEqual(new[1][1][2], 14)
        self.invoke("clear")
        self.assertEqual(len(datasets(self.invoke("scan", str(self.root)).stdout)), 1)

    def test_disabled_cache(self):
        self.invoke("scan", str(self.root), "--no-cache")
        self.assertFalse(self.profile.exists())

    def test_unchanged_scan_reuses_verified_preview(self):
        self.invoke("scan", str(self.root))
        saved, verified = datasets(self.invoke("scan", str(self.root)).stdout)
        self.assertEqual(saved[0], 6)
        self.assertEqual(verified, (8, []))

    def test_scanning_cache_parent_excludes_its_contents(self):
        with patch.dict(os.environ, {"LOCALAPPDATA": str(self.root), "XDG_CACHE_HOME": str(self.root)}):
            current, = datasets(self.invoke("scan", str(self.root)).stdout)
            cache_entry, = [r for r in current[1] if r[-1] == native("disk-diver")]
            self.assertEqual(cache_entry[0], 3)
            saved, verified = datasets(self.invoke("scan", str(self.root)).stdout)
            self.assertEqual(verified, (8, []))
            self.assertEqual(len(saved[1]), 3)

    def test_failed_scan_retains_old_generation(self):
        self.invoke("scan", str(self.root))
        @contextlib.contextmanager
        def failed(*_):
            yield io.BytesIO(b"DDV1\x05")
            raise OSError("simulated scanner failure after END")
        output = io.BytesIO()
        with patch.object(worker, "scanner", failed), self.assertRaises(OSError):
            worker.scan(self.root, False, False, "", output.write, lambda _: None)
        self.assertTrue(output.getvalue().endswith(b"DDV1\x07"))
        saved, _ = datasets(self.invoke("scan", str(self.root)).stdout)
        self.assertEqual(saved[1][1][2], 4)

    def test_process_cancellation_rolls_back(self):
        self.invoke("scan", str(self.root))
        (self.root / "groß.txt").write_bytes(b"changed")
        # Pause after BEGIN IMMEDIATE, deterministically exercise production cancellation.
        script = """
import worker, dd_privilege, sys, time, os
original = worker.Cache.begin
def hold(self, *args):
    original(self, *args)
    print('READY', file=sys.stderr, flush=True)
    time.sleep(60)
worker.Cache.begin = hold
dd_privilege.main(worker)
"""
        with subprocess.Popen([sys.executable, "-c", script, "scan", str(self.root)],
                              cwd=PROJECT, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                              stderr=subprocess.PIPE) as job:
            while line := job.stderr.readline():
                if line.strip() == b"READY":
                    break
            self.assertEqual(line.strip(), b"READY")
            _, _ = job.communicate(b"cancel\n", timeout=5)
            self.assertEqual(job.returncode, 130)
        self.assertEqual(len(datasets(self.invoke("scan", str(self.root)).stdout)), 2)

    def test_reconcile_renamed_directory_and_hardlinks(self):
        root = (1, journal.NO_PARENT, 0, 0, 1, 10, b"")
        old = [root, (1, 0, 0, 0, 1, 12, native("dir")),
               (2, 0, 4, 8, 1, 20, native("alias")),
               (2, 1, 4, 8, 1, 20, native("file"))]
        current = {self.root: [root, (1, 0, 0, 0, 1, 12, native("moved")),
                               (2, 0, 12, 16, 1, 20, native("alias"))],
                   self.root / "moved": [(1, journal.NO_PARENT, 0, 0, 1, 12, b""),
                                          (2, 0, 12, 16, 1, 20, native("file"))]}
        result = io.BytesIO()
        journal.reconcile(capture(old), result, self.root, ({10}, {20}), current.__getitem__)
        result.seek(0)
        rows = list(journal.records(result))
        self.assertEqual([row[6] for row in rows], [b"", native("moved"), native("alias"), native("file")])
        self.assertEqual([row[2] for row in rows if row[0] == 2], [12, 12])
        self.assertEqual(rows[-1][1], 1)

    def test_expired_journal_requires_refresh(self):
        volume = journal.Journal.__new__(journal.Journal)
        with self.assertRaises(ValueError):
            volume.changes("old journal", 20, ("new journal", 40, 19))
        with self.assertRaises(ValueError):
            volume.changes("same", 12, ("same", 40, 19))

    def test_large_sparse_file_and_cross_scope_hardlink(self):
        large = self.root / "sparse"
        size = (5 << 30) + 13
        with large.open("wb") as out:
            if os.name == "nt":
                import ctypes
                import msvcrt
                count = ctypes.c_uint32()
                api = ctypes.WinDLL("kernel32", use_last_error=True)
                ok = api.DeviceIoControl(ctypes.c_void_p(msvcrt.get_osfhandle(out.fileno())),
                                         0x900c4, None, 0, None, 0, ctypes.byref(count), None)
                if not ok:
                    self.skipTest("The test filesystem does not support sparse files")
            out.truncate(size)
        sub = self.root / "nested"
        sub.mkdir()
        os.link(large, sub / "alias")
        engine = worker.scanner_path(False)
        def summary(path):
            return json.loads(subprocess.check_output([engine, "summary", path]))
        self.assertEqual(summary(self.root)["logical"], size + 4)
        self.assertEqual(summary(sub)["logical"], size)
        self.assertLess(summary(sub)["allocated"], size)

    def test_partial_snapshot_does_not_get_a_journal_checkpoint(self):
        cache = worker.Cache(self.base / "partial.sqlite")
        self.addCleanup(cache.close)
        rows = [(1, journal.NO_PARENT, 0, 0, 1, 10, b""), (4, 0, 0, 0, 0, 0, b"")]
        cache.begin()
        cache.append(capture(rows).read())
        worker.finish_cache(cache, ("123", 456, 0), self.fail)
        self.assertIsNone(cache.metadata()[1])

    @unittest.skipUnless(os.name == "posix", "Unix cache mode")
    def test_cache_permissions(self):
        path = self.profile / "disk-diver"
        path.mkdir(parents=True)
        path.chmod(0o755)
        self.invoke("scan", str(self.root))
        self.assertEqual(path.stat().st_mode & 0o777, 0o700)


if __name__ == "__main__":
    unittest.main()
