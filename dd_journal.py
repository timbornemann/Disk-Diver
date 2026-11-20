"""NTFS journal checkpoints and targeted directory reconciliation for DDV1 snapshots."""

import ctypes
import os
import struct
from collections import defaultdict
from pathlib import Path

HEADER = struct.Struct("<BQQQQQI")
NO_PARENT = (1 << 64) - 1
# An open handle can extend, truncate or allocate storage again without a new
# record of the same reason. Revisit its first pending record until CLOSE.
SIZE_REASONS = 0x00000007 | 0x00008000 | 0x00020000 | 0x00040000
CLOSE = 0x80000000
FULL_SCAN_REASONS = 0x00010000 | 0x00000800 | 0x00100000
MAX_JOURNAL_BYTES = 128 * 1024 * 1024
MAX_OPEN_WRITERS = 32768


def records(stream):
    if stream.read(4) != b"DDV1":
        raise ValueError("Invalid capture header")
    while True:
        kind = stream.read(1)
        if kind == b"\x05":
            if stream.read(1):
                raise ValueError("Unexpected data after capture")
            return
        if kind not in (b"\x01", b"\x02", b"\x03", b"\x04"):
            raise ValueError("Incomplete capture")
        tail = stream.read(HEADER.size - 1)
        if len(tail) != HEADER.size - 1:
            raise ValueError("Truncated record")
        kind, parent, logical, allocated, dev, ino, length = HEADER.unpack(kind + tail)
        if length > 65536:
            raise ValueError("Invalid filename length")
        name = stream.read(length)
        if len(name) != length:
            raise ValueError("Truncated name")
        yield kind, parent, logical, allocated, dev, ino, name


class Journal:
    """Read an existing journal, without creating or altering it."""

    def __init__(self, root):
        from ctypes import wintypes as wt
        self.api = ctypes.WinDLL("kernel32", use_last_error=True)
        self.api.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, wt.LPVOID,
                                        wt.DWORD, wt.DWORD, wt.HANDLE]
        self.api.CreateFileW.restype = wt.HANDLE
        self.api.DeviceIoControl.argtypes = [wt.HANDLE, wt.DWORD, wt.LPVOID, wt.DWORD,
                                             wt.LPVOID, wt.DWORD, ctypes.POINTER(wt.DWORD), wt.LPVOID]
        self.api.CloseHandle.argtypes = [wt.HANDLE]
        self.handle = self.api.CreateFileW("\\\\.\\" + root.drive, 0x80000000, 7, None, 3, 0, None)
        if self.handle == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())

    def close(self):
        self.api.CloseHandle(self.handle)

    def request(self, code, data=b"", size=65536):
        output = ctypes.create_string_buffer(size)
        used = ctypes.c_uint32()
        if not self.api.DeviceIoControl(self.handle, code, data or None, len(data), output,
                                        size, ctypes.byref(used), None):
            raise ctypes.WinError(ctypes.get_last_error())
        return output.raw[:used.value]

    def checkpoint(self):
        # USN_JOURNAL_DATA_V0 starts with id, first, next, lowest valid.
        journal_id, first, end, lowest = struct.unpack_from("<Qqqq", self.request(0x900f4))
        return str(journal_id), end, max(first, lowest)

    def _events(self, start, checkpoint, ignore):
        journal_id, end, first = checkpoint
        if start is None or not first <= start <= end:
            raise ValueError("Journal history is unavailable")
        read_bytes = 0
        while start < end:
            query = struct.pack("<qIIQQQ", start, 0xffffffff, 0, 0, 0, int(journal_id))
            data = self.request(0x900bb, query)
            read_bytes += len(data)
            if read_bytes > MAX_JOURNAL_BYTES:
                raise ValueError("Journal history exceeds bounded scan")
            following, = struct.unpack_from("<q", data)
            if following <= start:
                raise ValueError("Journal cursor did not advance")
            offset = 8
            while offset < len(data):
                length, version, _, file_id, parent_id, usn = struct.unpack_from("<IHHQQq", data, offset)
                if version != 2 or length < 60 or offset + length > len(data):
                    raise ValueError("Unsupported journal record")
                if usn < start or usn >= following:
                    raise ValueError("Invalid journal record cursor")
                if usn < end:
                    reason, = struct.unpack_from("<I", data, offset + 40)
                    if not ignore or not ignore(file_id, parent_id):
                        yield file_id, parent_id, usn, reason
                offset += length
            start = following

    @staticmethod
    def _track(writers, file_id, usn, reason):
        if reason & CLOSE:
            writers.pop(file_id, None)
        elif reason & SIZE_REASONS:
            writers.setdefault(file_id, usn)
            if len(writers) > MAX_OPEN_WRITERS:
                raise ValueError("Too many unclosed journal records")

    def baseline_cursor(self, checkpoint, ignore=None):
        """Find pre-existing open writes visible in retained history, after a full scan."""
        _, end, first = checkpoint
        writers = {}
        try:
            for file_id, _, usn, reason in self._events(first, checkpoint, ignore):
                self._track(writers, file_id, usn, reason)
        except (OSError, ValueError, struct.error, OverflowError):
            return None
        return min(writers.values(), default=end)

    def changes(self, saved_id, start, checkpoint, ignore=None):
        if saved_id != checkpoint[0]:
            raise ValueError("Journal identity changed")
        parents, objects, writers = set(), set(), {}
        for file_id, parent_id, usn, reason in self._events(start, checkpoint, ignore):
            # A hardlink record can omit an alias parent. ACLs/reparse points can
            # change visibility below a directory, requiring a full traversal.
            if reason & FULL_SCAN_REASONS:
                raise ValueError("Changed namespace or permissions require traversal")
            parents.add(parent_id)
            objects.add(file_id)
            self._track(writers, file_id, usn, reason)
            if len(parents) > 256 or len(objects) > 32768:
                raise ValueError("A full traversal is cheaper for this change set")
        return (parents, objects), min(writers.values(), default=checkpoint[1])


def volume_journal(root, full):
    if os.name == "nt" and full and root == Path(root.anchor) and len(root.drive) == 2:
        try:
            return Journal(root)
        except OSError:
            pass
    return None


def reconcile(previous, output, root_path, changes, list_directory):
    """Rebuild a stream from cached listings plus freshly enumerated changed directories.

    Previous ordinals are local to the input generation. All output ordinals are new.
    A moved directory retains its children by identity. New directories are enumerated.
    """
    dirty_parents, changed_ids = changes
    dirty = set(dirty_parents)
    children = defaultdict(list)
    dir_ids = {}
    seen_dirs = set()
    root = None
    for index, row in enumerate(records(previous)):
        kind, parent, _, _, _, ino, _ = row
        if kind == 4 or not ino and kind in (1, 2):
            raise ValueError("Incomplete identities require a fresh scan")
        if index == 0:
            if kind != 1 or parent != NO_PARENT:
                raise ValueError("Invalid cached root")
            root = row
        else:
            owner = dir_ids[parent]
            children[owner].append(row)
            if ino in changed_ids:
                dirty.add(owner)  # Refresh every known hardlink location.
        if kind == 1:
            if ino in seen_dirs:
                raise ValueError("Ambiguous directory identity")
            dir_ids[index] = ino
            seen_dirs.add(ino)
    if root is None:
        raise ValueError("Missing root")

    def emit(row, parent):
        kind, _, logical, allocated, dev, ino, name = row
        output.write(HEADER.pack(kind, parent, logical, allocated, dev, ino, len(name)))
        output.write(name)

    output.write(b"DDV1")
    emit(root, NO_PARENT)
    next_id = 1
    pending = [(root[5], 0, root_path)]
    visited = set()
    while pending:
        ino, parent, path = pending.pop()
        if ino in visited:
            raise ValueError("Directory moved during refresh")
        visited.add(ino)
        if ino in dirty or ino not in seen_dirs:
            rows = list_directory(path)
            if not rows or rows[0][5] != ino or rows[0][4] != root[4]:
                raise ValueError("Directory identity changed during refresh")
            entries = rows[1:]
            if any(row[0] == 4 for row in entries):
                raise ValueError("Directory listing incomplete")
        else:
            entries = children.get(ino, ())
        for row in entries:
            emit(row, parent)
            if row[0] == 1:
                name = row[6].decode("utf-16-le", "surrogatepass") if os.name == "nt" else os.fsdecode(row[6])
                pending.append((row[5], next_id, path / name))
            next_id += 1
    output.write(b"\x05")
