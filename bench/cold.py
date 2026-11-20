"""Measure one scanner as the first access after preparing the OS cache externally."""

import argparse
import json
import platform
import sys
import tempfile
from pathlib import Path

from measure import invoke


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("root", type=Path)
    parser.add_argument("--state", choices=("first-after-reboot", "uncontrolled-first-run"), required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    # Do not enumerate the fixture, hash the scanner, or prime the metadata cache here.
    with tempfile.TemporaryFile() as output:
        sample = invoke([args.binary.resolve(), "summary", args.root], output, sys.stderr)
        output.seek(0)
        sample.update({"summary": json.load(output), "os": platform.system(), "cache_state": args.state, "binary": args.binary.name})
    args.output.write_text(json.dumps(sample, indent=2) + "\n", encoding="utf-8")
