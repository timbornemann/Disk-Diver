"""Compare both Release scanners and the production cache coordinator on one host."""

import argparse
import hashlib
import json
import mmap
import os
import platform
import statistics
import struct
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path

import fixture
from measure import invoke

CORES = ("cpp", "rust")
HERE = Path(__file__).resolve().parent


def summary(binary, root):
    return json.loads(subprocess.check_output([binary, "summary", root]))


def checked(reference, actual, case):
    if actual != reference:
        raise ValueError(f"{case}: unequal summaries\n{actual}\n{reference}")


def fresh_capture(source, target, expect_saved):
    # Read frame boundaries through record lengths, including arbitrary native names.
    with source.open("rb") as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
        offset = 0
        markers = []
        preview = None
        while offset < len(data):
            if data[offset:offset + 4] != b"DDV1" or offset + 5 > len(data):
                raise ValueError("invalid worker stream")
            markers.append(data[offset + 4])
            start = offset + 5
            offset = start
            while offset < len(data) and data[offset] in (1, 2, 3, 4):
                length, = struct.unpack_from("<I", data, offset + 41)
                offset += 45 + length
            if offset >= len(data) or data[offset] != 5:
                raise ValueError("incomplete worker frame")
            offset += 1
            if markers[-1] == 6:
                preview = (start, offset)
            elif markers[-1] == 7 or markers[-1] == 8:
                if markers[-1] == 8 and (not preview or start + 1 != offset):
                    raise ValueError("unexpected unchanged frame")
                begin, end_offset = preview if markers[-1] == 8 else (start, offset)
                with target.open("wb") as out:
                    out.write(b"DDV1")
                    while begin < end_offset:
                        end = min(begin + 1024 * 1024, end_offset)
                        out.write(data[begin:end])
                        begin = end
        if markers not in (([6, 7], [6, 8]) if expect_saved else ([7],)):
            raise ValueError(f"expected cached/fresh sequence, received {markers}")


def run(args, temp):
    root = args.fixture.resolve()
    manifest = fixture.read(root)
    full = args.full_root.resolve() if args.full_root else root / "tree"
    small = root / "tree" / "selected"
    if not small.is_relative_to(full):
        raise ValueError("the full volume must contain the fixture")
    if temp.is_relative_to(full) or args.output.resolve().is_relative_to(full):
        raise ValueError("choose --temp and --output outside the measured tree")
    programs = {core: getattr(args, core).resolve(strict=True) for core in CORES}
    worker = args.worker.resolve(strict=True)
    cases = {name: {core: [] for core in CORES} for name in ("full", "subtree", "refresh", "replay")}
    references = {"full": summary(programs["cpp"], full), "subtree": summary(programs["cpp"], small)}
    for name, tree in (("full", full), ("subtree", small)):
        checked(references[name], summary(programs["rust"], tree), name)
    if not args.full_root:
        want = sum(len(fixture.content(i)) for i in range(manifest["files"])) + sum(len(fixture.content(i)) for i in range(manifest["edit_count"]))
        if references["full"]["logical"] != want or references["full"]["files"] != manifest["files"] + manifest["edit_count"] + 1:
            raise ValueError("fixture does not match its logical-byte and file-count oracle")
    capture = temp / "shared.ddv"
    with capture.open("wb") as out:
        subprocess.run([programs["cpp"], "scan", full], stdout=out, check=True)
    for binary in programs.values():
        checked(references["full"], json.loads(subprocess.check_output([binary, "replay", capture])), "initial replay")

    envs = {}
    result = temp / "stdout.bin"
    fresh = temp / "latest.ddv"
    log = temp / "stderr.txt"
    refresh_modes = {}
    def timed(command, env=None):
        with result.open("wb") as out, log.open("wb") as err:
            try:
                return invoke(command, out, err, env)
            except RuntimeError as exc:
                err.flush()
                raise RuntimeError(log.read_text(encoding="utf-8", errors="replace")) from exc

    for core, binary in programs.items():
        base = temp / core
        base.mkdir()
        envs[core] = dict(os.environ, DISK_DIVER_SCANNER=str(binary), LOCALAPPDATA=str(base), XDG_CACHE_HOME=str(base), PYTHONUTF8="1")
        timed([sys.executable, worker, "scan", full], envs[core])
        fresh_capture(result, fresh, False)
        checked(references["full"], json.loads(subprocess.check_output([binary, "replay", fresh])), "cache seed")

    # Alternate which candidate runs first. Every case receives an untimed warmup.
    for n in range(args.rounds):
        order = CORES if n % 2 == 0 else CORES[::-1]
        for case, path, mode in (("full",full,"summary"), ("subtree",small,"summary"), ("replay",capture,"replay")):
            for core in order:
                cases[case][core].append(timed([programs[core], mode, path]))
                checked(references["subtree" if case == "subtree" else "full"], json.loads(result.read_bytes()), case)
                if getattr(args, "progress", False):
                    print(f"{case} {n+1}/{args.rounds} {core}: {cases[case][core][-1]['wall_ms']:.1f} ms", flush=True)

    changed = False
    try:
        for n in range(args.rounds):
            fixture.mutate(root, manifest, not changed)
            changed = not changed
            expected = summary(programs["cpp"], full)
            checked(expected, summary(programs["rust"], full), "edited tree")
            for core in CORES if n % 2 == 0 else CORES[::-1]:
                cases["refresh"][core].append(timed([sys.executable, worker, "scan", full], envs[core]))
                fresh_capture(result, fresh, True)
                checked(expected, json.loads(subprocess.check_output([programs[core], "replay", fresh])), "refresh")
                text = log.read_text(encoding="utf-8", errors="replace")
                refresh_modes[core] = "USN" if "NTFS journal checked" in text else "traversal"
                if getattr(args, "progress", False):
                    print(f"refresh {n+1}/{args.rounds} {core}: {cases['refresh'][core][-1]['wall_ms']:.1f} ms", flush=True)
    finally:
        if changed:
            fixture.mutate(root, manifest, False)

    others = {}
    external_versions = {}
    if args.gdu:
        external_versions["gdu"] = subprocess.check_output([args.gdu, "--version"], text=True).strip().splitlines()[0]
        samples = []
        export = temp / "gdu.json"
        for _ in range(args.rounds):
            if export.exists():
                export.unlink()
            # gdu's mount-point discovery (-x) is Linux-only. Windows volume
            # comparisons use a dedicated fixture with no nested mounts.
            traversal_flags = ["-x"] if sys.platform.startswith("linux") else []
            sample = timed([args.gdu, *traversal_flags, "-p", "-o", str(export), full])
            if not export.is_file() or export.stat().st_size == 0:
                raise ValueError("gdu returned an empty or missing export")
            sample["export_bytes"] = export.stat().st_size
            samples.append(sample)
        others["gdu_json_export"] = samples
    if args.wiztree:
        others["wiztree_csv_export"] = [timed([args.wiztree, full, f"/export={temp / 'wiztree.csv'}", "/admin=0"]) for _ in range(args.rounds)]
    qualified = bool(args.production and full.is_mount() and references["full"]["nodes"] >= 1_000_000 and references["full"]["complete"] and args.rounds >= 5 and args.hardware and args.filesystem and args.storage)
    return {"schema": 1, "timestamp": datetime.now(timezone.utc).isoformat(), "os": platform.system(), "os_detail": platform.platform(),
            "hardware": args.hardware, "storage": args.storage, "filesystem": args.filesystem, "rounds": args.rounds,
            "cache_state": "warm", "qualified": qualified, "fixture": manifest, "volume_root": full.is_mount(),
            "scope": "Scanner summary and replay include indexing. Refresh includes coordinator, cache, and streaming; GUI excluded.",
            "peak_memory": "Windows process peak working set; Linux wait4 ru_maxrss. Refresh involves a child scanner; metrics are not a combined process-tree total.",
            "binary_sha256": {k: hashlib.sha256(v.read_bytes()).hexdigest() for k, v in programs.items()},
            "worker_sha256": file_hash(worker),
            "capture_sha256": file_hash(capture), "summary": references, "refresh_modes": refresh_modes, "cases": cases, "external": others,
            "external_versions": external_versions}


def file_hash(path):
    digest = hashlib.sha256()
    with path.open("rb") as src:
        for chunk in iter(lambda: src.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ("cpp", "rust", "fixture", "output"):
        parser.add_argument("--" + flag, required=True, type=Path)
    parser.add_argument("--worker", type=Path, default=HERE.parent / "worker.py")
    parser.add_argument("--full-root", type=Path)
    parser.add_argument("--temp", type=Path, help="parent for the temporary cache/captures outside the scanned volume")
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--progress", action="store_true")
    parser.add_argument("--production", action="store_true", help="attest release builds and a quiet dedicated benchmark volume")
    for opt in ("hardware", "storage", "filesystem", "gdu", "wiztree"):
        parser.add_argument("--" + opt)
    opts = parser.parse_args()
    if opts.rounds < 1:
        parser.error("--rounds must be positive")
    with tempfile.TemporaryDirectory(dir=opts.temp, prefix="disk-diver-bench-") as folder:
        report = run(opts, Path(folder).resolve())
    opts.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    for case, pairs in report["cases"].items():
        print(case, {core: round(statistics.median(x["wall_ms"] for x in values), 2) for core, values in pairs.items()})
