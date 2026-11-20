"""Run inside the elevated, disposable Windows test installation only."""

import io
import json
import os
from pathlib import Path
import subprocess
import sys
import uuid

stage = Path(__file__).resolve().parent
if Path(sys.argv[1]).resolve(strict=True) != stage or not stage.name.startswith("Disk-Diver-Test-"):
    raise SystemExit("Run the staged test script from its disposable installation.")
sys.path.insert(0, str(stage))
import dd_journal as journal
import dd_privilege
import worker


def check(root):
    records = []
    owner = "journal-acceptance-" + uuid.uuid4().hex
    dd_privilege.enable_backup_privilege()

    def scan(label):
        output = io.BytesIO()
        messages = []
        worker.scan(root, True, False, owner, output.write, messages.append)
        data = output.getvalue()
        # Every call uses known data on the isolated test volume. Parse actual frames.
        stream = io.BytesIO(data)
        frames = []
        while stream.read(4) == b"DDV1":
            marker = stream.read(1)[0]
            current = io.BytesIO()
            current.write(b"DDV1")
            while True:
                first = stream.read(1)
                current.write(first)
                if first == b"\x05":
                    break
                tail = stream.read(journal.HEADER.size - 1)
                _, _, _, _, _, _, n = journal.HEADER.unpack(first + tail)
                current.write(tail + stream.read(n))
            frames.append(marker)
            if marker in (6, 7):
                latest = current.getvalue()
        capture = stage / "acceptance.ddv"
        capture.write_bytes(latest)
        exe = worker.scanner_path(True)
        indexed = json.loads(subprocess.check_output([exe, "replay", capture]))
        fresh = json.loads(subprocess.check_output([exe, "summary", root]))
        if fresh != indexed:
            raise AssertionError((label, indexed, fresh))
        records.append(dict(case=label, messages=messages, frames=frames, summary=indexed))
        return messages

    scan("baseline")
    untouched = scan("unchanged")
    target = root / "acceptance" / "edited"
    target.write_bytes(b"journal acceptance" * 4096)
    refreshed = scan("create")
    target.rename(target.with_suffix(".moved"))
    renamed = scan("rename")
    target.with_suffix(".moved").unlink()
    removed = scan("delete")
    for case in (untouched, refreshed, renamed, removed):
        if not any("NTFS journal checked" in line for line in case):
            raise AssertionError(case)
    with target.open("wb") as out:
        for case in ("open-write", "repeated-write"):
            out.write(b"still open" * 4096)
            out.flush()
            os.fsync(out.fileno())
            scan(case)
    scan("closed-write")
    target.unlink()
    scan("final-delete")
    return records


if __name__ == "__main__":
    root = Path(sys.argv[2])
    if stage.parent != dd_privilege.program_files() or not stage.name.startswith("Disk-Diver-Test-"):
        raise SystemExit("Requires the isolated acceptance installation.")
    ready = json.loads((stage / "ready.json").read_bytes())
    if Path(ready["root"]) != root or not (root / "acceptance" / "resident").is_file():
        raise SystemExit("Requires the isolated test volume.")
    print(json.dumps(check(root), indent=2))
