"""Exercise one fresh scan through the worker and scanner from the staged package."""

import argparse
import json
import os
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("app", type=Path)
    parser.add_argument("--update-platform")
    args = parser.parse_args()
    app = args.app.resolve(strict=True)
    if args.update_platform:
        config = json.loads((app / "disk-diver-update.json").read_text(encoding="utf-8"))
        if config.get("platform") != args.update_platform:
            raise ValueError(f"build with -DDISK_DIVER_UPDATE_PLATFORM={args.update_platform}")
        # The installer imports its adjacent updater. Import the same signature
        # verification backend without fetching a release or starting polkit.
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey  # noqa: F401
        runpy.run_path(str(app / "install_update.py"))
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
