"""Disk-Diver local scan coordinator. Binary stdout is reserved for the Qt client."""

import contextlib
import datetime
import hashlib
import io
import itertools
import os
from pathlib import Path
import sqlite3
import struct
import subprocess
import shutil
import sys
import tempfile
import threading

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))  # Also works with the privileged interpreter's isolated mode.
import dd_journal as journal

CHUNK = 256 * 1024
ACTIVE = set()
ACTIVE_LOCK = threading.Lock()


def cancel():
    with ACTIVE_LOCK:
        for child in ACTIVE:
            with contextlib.suppress(OSError):
                child.kill()
    os._exit(130)  # SQLite rolls back open transactions on the next connection.


def watch_stdin():
    if sys.stdin and not sys.stdin.isatty():
        pending = b""
        while data := os.read(sys.stdin.fileno(), 1024):
            pending += data
            lines = pending.split(b"\n")
            pending = lines.pop()[-16:]
            if any(line.strip() == b"cancel" for line in lines):
                cancel()


def status(message):
    print("STATUS " + str(message).replace("\n", " "), file=sys.stderr, flush=True)


def scanner_path(full):
    directories = [HERE]
    if not full:
        if os.getenv("DISK_DIVER_SCANNER"):
            return Path(os.environ["DISK_DIVER_SCANNER"]).resolve(strict=True)
        if os.getenv("DISK_DIVER_BIN_DIR"):
            directories.append(Path(os.environ["DISK_DIVER_BIN_DIR"]))
        directories.extend(HERE / "build" / "bin" / subdir for subdir in ("Release", ""))
    for directory in directories:
        config = directory / "disk-diver-engine.txt"
        engine = config.read_text("ascii").strip() if config.is_file() else "cpp"
        if engine not in ("cpp", "rust"):
            raise OSError("Invalid installed scanner selection")
        path = directory / ("disk-diver-" + engine + (".exe" if os.name == "nt" else ""))
        if path.is_file():
            return path.resolve()
    raise OSError("Scanner executable missing. Build and install the engine next to worker.py.")


@contextlib.contextmanager
def scanner(executable, operation, root, skip=None):
    env = os.environ.copy()
    env.pop("DISK_DIVER_SKIP_DIR_IDS", None)
    if skip:
        env["DISK_DIVER_SKIP_DIR_IDS"] = f"{skip[0]}:{skip[1]}"
    child = subprocess.Popen([str(executable), operation, str(root)], stdin=subprocess.DEVNULL,
                             stdout=subprocess.PIPE, stderr=None, cwd=executable.parent, env=env)
    with ACTIVE_LOCK:
        ACTIVE.add(child)
    try:
        yield child.stdout
        if child.wait() != 0:
            raise OSError("The scanner stopped before completing the scan.")
    finally:
        if child.poll() is None:
            child.kill()
        child.stdout.close()
        child.wait()
        with ACTIVE_LOCK:
            ACTIVE.discard(child)


def private_cache(full, owner):
    if full:
        if os.name == "nt":
            base = HERE / "private-cache"
            from dd_privilege import protected_directory
            protected_directory(base)
        else:
            base = Path("/var/cache/disk-diver")
            base.mkdir(mode=0o700, exist_ok=True)
            meta = base.lstat()
            if base.is_symlink() or meta.st_uid != 0 or meta.st_mode & 0o077:
                raise OSError("Full-scan cache must be root-owned with mode 0700.")
        if not base.is_dir():
            raise OSError("Install the protected full-scan cache first.")
        account = base / hashlib.sha256(owner.encode("utf8")).hexdigest()
        account.mkdir(mode=0o700, exist_ok=True)
        return account
    if os.name == "nt":
        base = Path(os.environ["LOCALAPPDATA"])
    else:
        base = Path(os.getenv("XDG_CACHE_HOME") or Path.home() / ".cache")
    base = base / "disk-diver"
    base.mkdir(mode=0o700, parents=True, exist_ok=True)
    if os.name != "nt":
        meta = base.lstat()
        if base.is_symlink() or meta.st_uid != os.geteuid() or not base.is_dir():
            raise OSError("The scan cache must be a private directory owned by this user.")
        if meta.st_mode & 0o077:
            base.chmod(0o700)
    return base


def cache_name(root, owner):
    info = root.stat()
    identity = f"DDV1.1/{owner}/{info.st_dev}/{info.st_ino}/".encode() + os.fsencode(root)
    return hashlib.sha256(identity).hexdigest() + ".sqlite3"


def cache_identity(path):
    if os.name != "nt":
        info = path.stat()
        return info.st_dev, info.st_ino
    import ctypes
    from ctypes import wintypes as wt

    class FileInfo(ctypes.Structure):
        _fields_ = [("attributes", wt.DWORD), ("created", wt.FILETIME), ("accessed", wt.FILETIME),
                    ("written", wt.FILETIME), ("volume", wt.DWORD), ("size_hi", wt.DWORD),
                    ("size_lo", wt.DWORD), ("links", wt.DWORD), ("id_hi", wt.DWORD), ("id_lo", wt.DWORD)]

    api = ctypes.WinDLL("kernel32", use_last_error=True)
    api.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, wt.LPVOID,
                                wt.DWORD, wt.DWORD, wt.HANDLE]
    api.CreateFileW.restype = wt.HANDLE
    api.GetFileInformationByHandle.argtypes = [wt.HANDLE, ctypes.POINTER(FileInfo)]
    api.CloseHandle.argtypes = [wt.HANDLE]
    handle = api.CreateFileW(str(path), 0, 7, None, 3, 0x02000000, None)
    if handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        info = FileInfo()
        if not api.GetFileInformationByHandle(handle, ctypes.byref(info)):
            raise ctypes.WinError(ctypes.get_last_error())
        return info.volume, info.id_hi << 32 | info.id_lo
    finally:
        api.CloseHandle(handle)


def cache_filter(root, skip):
    if skip and skip[1] and cache_identity(root)[0] == skip[0]:
        return lambda file_id, parent: file_id == skip[1] or parent == skip[1]
    return None


def fast_volume_capture(executable, root, full, owner, cleanup, report, skip):
    if os.name != "nt" or not full or root != Path(root.anchor) or len(root.drive) != 2:
        return None
    config = executable.parent / "disk-diver-ntfs-layout.txt"
    if not config.exists() or config.read_text("ascii").strip() != "ON":
        return None
    try:
        capture = cleanup.enter_context(tempfile.TemporaryFile(dir=private_cache(True, owner)))
        with scanner(executable.parent / "disk-diver-cpp.exe", "layout", root, skip) as source:
            shutil.copyfileobj(source, capture, CHUNK)
        capture.seek(0)
        return capture
    except OSError as exc:
        report("Volume query unavailable, using directory enumeration: " + str(exc))
        return None


class ChunkReader(io.RawIOBase):
    """Buffer a cursor of SQLite chunks without copying the snapshot to disk."""
    def __init__(self, chunks):
        self.chunks = iter(chunks)
        self.pending = memoryview(b"")

    def readable(self):
        return True

    def readinto(self, buffer):
        while not self.pending:
            block = next(self.chunks, None)
            if block is None:
                return 0
            self.pending = memoryview(block)
        count = min(len(buffer), len(self.pending))
        buffer[:count] = self.pending[:count]
        self.pending = self.pending[count:]
        return count


class Cache:
    def __init__(self, path):
        self.db = sqlite3.connect(path, timeout=0.2)
        try:
            self.db.executescript("""
                CREATE TABLE IF NOT EXISTS chunks (seq INTEGER PRIMARY KEY, data BLOB NOT NULL);
                CREATE TABLE IF NOT EXISTS metadata (
                    id INTEGER PRIMARY KEY CHECK(id=1), stamp TEXT, journal_id TEXT, cursor INTEGER
                );
            """)
        except Exception:
            self.db.close()
            raise

    def metadata(self):
        self.db.execute("BEGIN")
        row = self.db.execute("SELECT stamp,journal_id,cursor FROM metadata WHERE id=1").fetchone()
        if row:
            # Legacy snapshots lacked the completeness guard and open-write cursor.
            stamp, jid, cursor = row
            return stamp, jid[6:] if jid and jid.startswith("clean:") else None, cursor

    def chunks(self):
        for (chunk,) in self.db.execute("SELECT data FROM chunks ORDER BY seq"):
            yield chunk

    def reader(self):
        return io.BufferedReader(ChunkReader(self.chunks()), buffer_size=CHUNK)

    def compare(self, stream):
        """Return the first differing row and its fresh bytes, or None for equality.

        Row 1 contains the separately checked DDV1 magic. The read transaction
        pins the same generation that the user was shown.
        """
        for seq, saved in self.db.execute("SELECT seq,data FROM chunks WHERE seq>1 ORDER BY seq"):
            fresh = stream.read(len(saved))
            if fresh != saved:
                return seq, fresh
        if stream.read(1):
            raise OSError("Data after the end of a cached scanner stream")
        return None

    def prefix(self, boundary):
        for (data,) in self.db.execute("SELECT data FROM chunks WHERE seq>1 AND seq<? ORDER BY seq",
                                       (boundary,)):
            yield data

    def begin(self, boundary=0):
        if not self.db.in_transaction:
            self.db.execute("BEGIN IMMEDIATE")
        self.db.execute("DELETE FROM chunks WHERE seq>=?", (boundary,))

    def append(self, block):
        self.db.execute("INSERT INTO chunks(data) VALUES (?)", (block,))

    def finish(self, checkpoint):
        jid, cursor = checkpoint[:2] if checkpoint else (None, None)
        stamp = datetime.datetime.now().astimezone().isoformat(timespec="seconds")
        self.db.execute("INSERT OR REPLACE INTO metadata VALUES(1,?,?,?)",
                        (stamp, ("clean:" + jid) if jid else None, cursor))
        self.db.commit()

    def close(self):
        self.db.close()


def finish_cache(cache, checkpoint, report, clean=False):
    if cache:
        try:
            if checkpoint and not clean:
                with cache.reader() as source:
                    for kind, _, _, allocated, _, ino, _ in journal.records(source):
                        if kind == 4 or kind in (1, 2) and (not ino or allocated == journal.NO_PARENT):
                            checkpoint = None
                            break
            cache.finish(checkpoint)
        except sqlite3.Error as exc:
            cache.db.rollback()
            report("Could not save this scan: " + str(exc))


def publish(stream, active_scan, cache, previous, checkpoint, write, report):
    if stream.read(4) != b"DDV1":
        raise OSError("Invalid or missing scanner header")
    boundary, first = 0, b""
    if previous:
        changed = cache.compare(stream)
        if changed is None:
            active_scan.close()
            finish_cache(cache, checkpoint, report, clean=bool(previous[1]))
            write(b"DDV1\x08\x05")
            return
        boundary, first = changed
    write(b"DDV1\x07")
    tail = b""
    if boundary:
        for block in cache.prefix(boundary):
            payload = tail + block
            tail = payload[-1:]
            write(payload[:-1])
    if cache:
        try:
            cache.begin(boundary)
            if not boundary:
                cache.append(b"DDV1")
        except sqlite3.Error as exc:
            cache.db.rollback()
            report("Cache write unavailable: " + str(exc))
            cache = None
    # A dataset becomes complete only after the scanner exits successfully.
    read = getattr(stream, "read1", stream.read)
    for block in itertools.chain((first,), iter(lambda: read(CHUNK), b"")):
        if not block:
            continue
        payload = tail + block
        tail = payload[-1:]
        write(payload[:-1])
        if cache:
            try:
                cache.append(block)
            except sqlite3.Error as exc:
                cache.db.rollback()
                cache = None
                report("Cache write unavailable: " + str(exc))
    if tail != b"\x05":
        raise OSError("Incomplete scan")
    active_scan.close()
    finish_cache(cache, checkpoint, report)
    write(tail)


def scan(root, full, no_cache, owner, write, report):
    executable = scanner_path(full)
    root = Path(os.path.abspath(root))
    if not root.is_dir() or root.is_symlink() or getattr(root, "is_junction", lambda: False)():
        raise OSError("Choose an accessible directory or drive root.")
    with contextlib.ExitStack() as cleanup:
        cache = None
        previous = None
        skip = None
        directory = None
        if not no_cache:
            try:
                directory = private_cache(full, owner)
                if not root.is_relative_to(directory):
                    cache = Cache(directory / cache_name(root, owner))
                    cleanup.callback(cache.close)
                    skip = cache_identity(directory)
                    previous = cache.metadata()
                    if previous:
                        report("Saved scan · " + previous[0])
                        play(cache.chunks(), 6, write)
            except (OSError, sqlite3.Error) as exc:
                report("Cache unavailable: " + str(exc))
                cache = previous = None

        volume = journal.volume_journal(root, full) if cache else None
        checkpoint = refreshed = None
        if volume:
            cleanup.callback(volume.close)
            try:
                checkpoint = volume.checkpoint()
                if previous and previous[1]:
                    ignore = cache_filter(root, skip)
                    changes, safe_cursor = volume.changes(previous[1], previous[2], checkpoint, ignore)
                    checkpoint = (checkpoint[0], safe_cursor, checkpoint[2])
                    if not any(changes):
                        report("NTFS journal checked · no changes")
                        finish_cache(cache, checkpoint, report, clean=True)
                        write(b"DDV1\x08\x05")
                        return
                    # All temporary data uses the same protected directory as the full cache.
                    with cache.reader() as old:
                        refreshed = cleanup.enter_context(tempfile.TemporaryFile(dir=directory))

                        def listing(path):
                            with scanner(executable, "list", path, skip) as stream:
                                return list(journal.records(stream))

                        journal.reconcile(old, refreshed, root, changes, listing)
                        refreshed.seek(0)
                        report("NTFS journal checked · changed directories refreshed")
                else:
                    start = volume.baseline_cursor(checkpoint, cache_filter(root, skip))
                    checkpoint = (checkpoint[0], start, checkpoint[2]) if start is not None else None
            except (OSError, ValueError, KeyError, sqlite3.Error, OverflowError, struct.error) as exc:
                checkpoint = None
                refreshed = None
                report("Refreshing the selected tree: " + str(exc))

        active_scan = cleanup.enter_context(contextlib.ExitStack())
        if refreshed is None:
            refreshed = fast_volume_capture(executable, root, full, owner, cleanup, report, skip)
        stream = refreshed or active_scan.enter_context(scanner(executable, "scan", root, skip))
        publish(stream, active_scan, cache, previous, checkpoint, write, report)


def play(chunks, marker, write):
    first = True
    for block in chunks:
        if first:
            if not block.startswith(b"DDV1"):
                raise OSError("Invalid cached scan header")
            write(b"DDV1" + bytes([marker]))
            block = block[4:]
            first = False
        if block:
            write(block)
    if first:
        raise OSError("Empty cached scan")


def clear(full, owner, report):
    # Remove only our hashed snapshot files, never a caller-supplied path.
    import re
    for path in private_cache(full, owner).iterdir():
        if re.fullmatch(r"[0-9a-f]{64}\.sqlite3", path.name) and path.is_file():
            db = sqlite3.connect(path, timeout=0.2)
            try:
                db.execute("BEGIN EXCLUSIVE")
                db.execute("DELETE FROM chunks")
                db.execute("DELETE FROM metadata")
                db.commit()
                db.execute("VACUUM")
            finally:
                db.close()
    report("Saved scans cleared")


def run(operation, root, full, no_cache, owner, write, report):
    if operation == "clear":
        clear(full, owner, report)
    else:
        scan(root, full, no_cache, owner, write, report)


if __name__ == "__main__":
    if sys.stderr:
        sys.stderr.reconfigure(encoding="utf-8", errors="backslashreplace")
    from dd_privilege import main
    main(sys.modules[__name__])
