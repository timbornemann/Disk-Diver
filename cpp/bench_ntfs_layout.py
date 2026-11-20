"""Run from the protected VHD test installation, with both scanners and bench/measure.py beside this file."""

import argparse
import ctypes
import hashlib
import itertools
import json
import os
from pathlib import Path
import statistics
import struct
import subprocess
import sys


def canonical_records(command, records):
    # Stream ordinals vary between backends. A directory's volume + file ID identifies
    # its parent relationship on NTFS. Retain every native name and every size bit.
    parents = {}
    rows = []
    counts = {"directories": 0, "files": 0, "other": 0}
    kinds = ("", "directories", "files", "other")
    frame = struct.Struct("<B6QI")
    with subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE) as child:
        try:
            for ordinal, (kind, parent, logical, allocated, dev, ino, name) in enumerate(records(child.stdout)):
                if kind == 4 or allocated == (1 << 64) - 1:
                    raise ValueError(f"incomplete record at ordinal {ordinal}, parent {parent}")
                if ordinal == 0:
                    if kind != 1 or name or parent != (1 << 64) - 1:
                        raise ValueError("invalid root")
                    up = (0, 0)
                else:
                    up = parents[parent]
                if kind == 1:
                    if not ino:
                        raise ValueError("NTFS directory has no stable ID")
                    parents[ordinal] = (dev, ino)
                rows.append(frame.pack(kind, *up, logical, allocated, dev, ino, len(name)) + name)
                counts[kinds[kind]] += 1
            if child.wait():
                raise RuntimeError(f"{command[0]} exited with {child.returncode}")
        finally:
            if child.poll() is None:
                child.kill()
            child.stdout.close()
    if not rows:
        raise ValueError("no root")
    rows.sort()
    return rows, counts


def run(stage, root, rounds, minimum):
    import dd_privilege
    from dd_journal import records
    from measure import invoke

    if not dd_privilege.trusted(stage / "bench_ntfs_layout.py") or not ctypes.windll.shell32.IsUserAnAdmin():
        raise RuntimeError("launch the protected VHD benchmark with administrator rights")
    label = ctypes.create_unicode_buffer(256)
    if (not ctypes.windll.kernel32.GetVolumeInformationW(str(root), label, 256, None, None, None, None, 0)
            or label.value != "DiskDiverTest" or root != Path(root.anchor)):
        raise ValueError("expected the DiskDiverTest VHD drive root")
    dd_privilege.enable_backup_privilege()
    cpp = str((stage / "disk-diver-cpp.exe").resolve(strict=True))
    rust = str((stage / "disk-diver-rust.exe").resolve(strict=True))
    commands = {"cpp": [cpp, "scan", str(root)], "rust": [rust, "scan", str(root)], "layout": [cpp, "layout", str(root)]}
    print("Validating all native records against C++ traversal", file=sys.stderr, flush=True)
    reference, counts = canonical_records(commands["cpp"], records)
    if len(reference) < minimum:
        raise ValueError(f"only {len(reference)} entries (minimum {minimum})")
    for name in ("rust", "layout"):
        candidate, totals = canonical_records(commands[name], records)
        if totals != counts or candidate != reference:
            diff = next((i for i, (a, b) in enumerate(itertools.zip_longest(reference, candidate)) if a != b), None)
            raise ValueError(f"{name} differs at sorted record {diff}: reference counts {counts}, candidate {totals}")
        del candidate
    digest = hashlib.sha256()
    for row in reference:
        digest.update(row)
    del reference

    # Each method occupies each position equally often in a set of six rounds.
    orders = list(itertools.permutations(commands))
    samples = {name: [] for name in commands}
    schedule = []
    for round_number in range(rounds):
        order = orders[round_number % len(orders)]
        print(f"Round {round_number + 1}/{rounds}: {', '.join(order)}", file=sys.stderr, flush=True)
        schedule.append(order)
        for name in order:
            samples[name].append(invoke(commands[name], subprocess.DEVNULL, None))
    return {
        "volume_label": label.value, "matched_records": counts, "canonical_sha256": digest.hexdigest(),
        "warm_cache": True, "sink": "DEVNULL", "backup_privilege": True, "order": schedule,
        "samples": samples,
        "median_wall_ms": {name: statistics.median(row["wall_ms"] for row in values) for name, values in samples.items()},
        "peak_rss_bytes": {name: max(row["peak_rss_bytes"] for row in values) for name, values in samples.items()},
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--rounds", type=int, default=6)
    parser.add_argument("--min-nodes", type=int, default=1_000_000)
    args = parser.parse_args()
    if os.name != "nt" or args.rounds < 1 or args.min_nodes < 1:
        parser.error("requires Windows, a positive number of rounds and a minimum node count")
    directory = Path(__file__).resolve().parent
    sys.path.insert(0, str(directory))  # Trusted staged copy, also in Python isolated mode.
    print(json.dumps(run(directory, args.root.resolve(), args.rounds, args.min_nodes), indent=2))
