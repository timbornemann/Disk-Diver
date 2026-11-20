"""Apply the architecture decision to a Windows report and a Linux report."""

import argparse
import json
import statistics
from pathlib import Path


CASES = ("full", "subtree", "refresh")
CORES = ("cpp", "rust")


def select(reports, after_profile=False):
    if len(reports) != 2 or {r["os"] for r in reports} != {"Windows", "Linux"}:
        return {"winner": None, "reason": "One Windows and one Linux report required."}
    if not all(r["qualified"] for r in reports):
        return {"winner": None, "reason": "Smoke, partial or uncontrolled inputs. Production comparison pending."}
    fingerprints = {r.get("worker_sha256") for r in reports}
    if None in fingerprints or len(fingerprints) != 1:
        return {"winner": None, "reason": "Use the same cache coordinator revision on both platforms."}
    ratios = {core: [] for core in CORES}
    for report in reports:
        for case in CASES:
            times = {core: statistics.median(s["wall_ms"] for s in report["cases"][case][core]) for core in CORES}
            for core in CORES:
                ratios[core].append(times[core] / min(times.values()))
    worst = {core: max(ratios[core]) for core in CORES}
    average = {core: statistics.geometric_mean(ratios[core]) for core in CORES}
    eligible = [core for core in CORES if worst[core] <= 1.15 + 1e-12]
    if len(eligible) == 1:
        winner, reason = eligible[0], "Only eligible engine across all six cases."
    elif len(eligible) == 2:
        if max(average.values()) / min(average.values()) < 1.10 - 1e-12:
            winner, reason = "cpp", "Geometric means differ by less than 10%."
        else:
            winner, reason = min(CORES, key=average.get), "Faster geometric mean."
    elif not after_profile:
        winner, reason = None, "Both exceed the 15% case guardrail. Profile once, rerun and compare the new reports."
    else:
        winner, reason = min(CORES, key=worst.get), "Smallest worst normalized slowdown after profiling; exact tie favors C++."
    return {"winner": winner, "reason": reason, "normalized_geomeans": average, "maximum_slowdown": worst}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reports", nargs=2, type=Path)
    parser.add_argument("--after-profile", action="store_true")
    args = parser.parse_args()
    print(json.dumps(select([json.loads(p.read_text(encoding="utf-8")) for p in args.reports], args.after_profile), indent=2))
