"""Entry point for a staged Windows benchmark on an existing disposable VHD."""

import json
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace


def main():
    bench = Path(__file__).resolve().parent
    sys.path.insert(0, str(bench))  # The embedded Python runtime uses an isolated ._pth.
    import fixture
    import run

    stage = bench.parent
    volume = Path(json.loads((stage / "ready.json").read_text(encoding="utf-8-sig"))["root"])
    if not volume.is_mount() or volume.drive.lower() == os.environ["SystemDrive"].lower():
        raise ValueError("expected the mounted disposable test volume")
    work = bench / "scratch"
    work.mkdir()
    os.environ["TEMP"] = os.environ["TMP"] = str(work)
    os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
    args = SimpleNamespace(fixture=volume / "production", full_root=volume, cpp=bench / "disk-diver-cpp.exe",
                           rust=bench / "disk-diver-rust.exe", worker=bench / "worker.py", output=bench / "scale-windows.json",
                           production=True, rounds=5, gdu=str(bench / "gdu.exe"), wiztree=None,
                           hardware=os.environ["DD_BENCH_HARDWARE"], storage=os.environ["DD_BENCH_STORAGE"],
                           filesystem="NTFS")
    if os.environ.get("DD_BENCH_REUSE_FIXTURE") == "1":
        manifest = fixture.read(args.fixture)
        if manifest["files"] != 1_000_000 or manifest["edit_count"] != 1_000:
            raise ValueError("expected the completed production benchmark fixture")
    else:
        print("Generating one million files", flush=True)
        fixture.create(args.fixture, 1_000_000)
    print("Fixture complete. Measuring five warm rounds", flush=True)
    with tempfile.TemporaryDirectory(dir=work) as temp:
        args.progress = True
        result = run.run(args, Path(temp))
    result["authority"] = "Both engines and the ordinary traversal/SQLite coordinator ran with the same elevated token."
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print("Finished", args.output, flush=True)


if __name__ == "__main__":
    main()
