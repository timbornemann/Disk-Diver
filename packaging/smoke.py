"""Exercise one fresh scan through the worker and scanner from the staged package."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    app = Path(sys.argv[1]).resolve(strict=True)
    scanner_name = "disk-diver-" + (app / "disk-diver-engine.txt").read_text().strip()
    scanner = app / (scanner_name + (".exe" if os.name == "nt" else ""))
    env = {k: v for k, v in os.environ.items() if not k.startswith("DISK_DIVER_")}
    if os.name == "nt":
        env["PATH"] = os.path.join(os.environ["SystemRoot"], "System32")
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp) / "scan"
        root.mkdir()
        (root / "file").write_bytes(b"Disk-Diver")
        data = subprocess.check_output(
            [sys.executable, "-I", "-S", "-B", str(app / "worker.py"), "scan", str(root), "--no-cache"],
            env=env, stdin=subprocess.DEVNULL, timeout=30,
        )
        if not data.startswith(b"DDV1\x07") or data[-1:] != b"\x05":
            raise ValueError("installed worker did not produce a complete fresh scan")
        capture = Path(tmp) / "result.ddv1"
        capture.write_bytes(data[:4] + data[5:])
        summary = json.loads(subprocess.check_output([str(scanner), "replay", str(capture)], env=env, timeout=30))
        if summary["files"] != 1 or summary["logical"] != 10 or not summary["complete"]:
            raise ValueError(f"incorrect installed scanner result: {summary}")


if __name__ == "__main__":
    main()
