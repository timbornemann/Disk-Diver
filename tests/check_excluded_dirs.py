"""Run the same directory-identity fixture against either native scanner."""

import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from dd_journal import records


def scan(scanner, root, exclusions=""):
    result = subprocess.run([scanner, "scan", root], check=True, capture_output=True,
                            env=os.environ | {"DISK_DIVER_SKIP_DIR_IDS": exclusions})
    return list(records(io.BytesIO(result.stdout)))


def check(scanner):
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        cache = root / "cache"
        cache.mkdir()
        (cache / "hidden").write_bytes(b"12345")
        (root / "visible").write_bytes(b"xyz")
        rows = scan(scanner, root)
        names = {row[6].decode("utf-16-le" if os.name == "nt" else "utf-8"): row for row in rows}
        skip = ",".join(f"{names[name][4]}:{names[name][5]}" for name in ("cache", "visible", ""))
        result = scan(scanner, root, skip)
        assert sorted(row[0] for row in result) == [1, 2, 3], result
        leaf, = (row for row in result if row[0] == 3)
        assert leaf[2:6] == (0, 0, *names["cache"][4:6])
        assert sum(row[2] for row in result) == 3
        explicit = scan(scanner, cache, skip)
        assert [r[0] for r in explicit] == [1, 2]
        assert explicit[1][2] == 5
        different_volume = scan(scanner, root, f"{names['cache'][4] ^ 1}:{names['cache'][5]}")
        assert different_volume == rows


if __name__ == "__main__":
    check(sys.argv[1])
