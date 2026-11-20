"""Build a deterministic benchmark fixture in a new directory."""

import argparse
import json
import os
import stat
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path


MANIFEST = "disk-diver-fixture.json"
VERSION = 1


def content(i):
    return bytes([i % 251]) * (i % 113)


def location(root, selected, i):
    group, index = ("selected", i) if i < selected else ("bulk", i - selected)
    return root / "tree" / group / f"b{index // 16384:04}" / f"d{index // 128:06}" / f"f{index:08}"


def create(root, count):
    if count < 32:
        raise ValueError("choose at least 32 files")
    root.mkdir(parents=True, exist_ok=False)
    selected = min(2_048, max(8, count // 100))
    previous = None
    for i in range(count):
        path = location(root, selected, i)
        if path.parent != previous:
            path.parent.mkdir(parents=True, exist_ok=True)
            previous = path.parent
        path.write_bytes(content(i))
    # The link name crosses the boundary of the selected subtree.
    os.link(location(root, selected, 0), root / "tree" / "bulk" / "linked-file")
    (root / "tree" / "empty").mkdir()
    edits = root / "tree" / "changes"
    edits.mkdir()
    edit_count = min(1_000, max(4, count // 1_000))
    for i in range(edit_count):
        (edits / f"before-{i:04}").write_bytes(content(i))
    data = {"version": VERSION, "files": count, "selected_files": selected, "edit_count": edit_count}
    (root / MANIFEST).write_text(json.dumps(data) + "\n", encoding="utf-8")
    return data


def read(root):
    manifest = json.loads((root / MANIFEST).read_text(encoding="utf-8"))
    if manifest.get("version") != VERSION or not (4 <= manifest["edit_count"] <= 1_000):
        raise ValueError("unsupported benchmark fixture")
    return manifest


def mutate(root, state, changed):
    """Toggle owned files only; check identity/content before touching each one."""
    edits = root / "tree" / "changes"
    if edits.resolve() != edits:
        raise ValueError("fixture changes directory was redirected")
    for i in range(state["edit_count"]):
        before = edits / f"before-{i:04}"
        after = edits / (f"after-{i:04}" if i % 2 else before.name)
        old, new = (before, after) if changed else (after, before)
        original, modified = content(i), content(i) + b"Disk-Diver benchmark delta"
        expected, replacement = (original, modified) if changed else (modified, original)
        if old.is_symlink() or old.stat().st_nlink != 1 or old.read_bytes() != expected:
            raise ValueError(f"unexpected fixture file: {old}")
        if old != new:
            if os.path.lexists(new):
                raise ValueError(f"rename destination already exists: {new}")
            old.rename(new)
        new.write_bytes(replacement)


def remove(root):
    """Remove only generated paths with their original contents; preserve other files."""
    root = root.absolute()
    if root != root.resolve(strict=True) or root == Path(root.anchor):
        raise ValueError("Expected a direct path to a generated fixture directory")
    state = read(root)
    total, selected = state["files"], state["selected_files"]
    if total < 32 or selected != min(2048, max(8, total // 100)):
        raise ValueError("Invalid fixture manifest")

    def verify_directory(path):
        if path.resolve() != path or not path.is_relative_to(root):
            raise ValueError(f"Redirected fixture directory: {path}")

    def remove_file(path, original):
        if not os.path.lexists(path):
            return
        meta = path.lstat()
        if (not stat.S_ISREG(meta.st_mode) or getattr(meta, "st_file_attributes", 0) & 0x400 or
                meta.st_size != len(original) or path.read_bytes() != original):
            raise ValueError(f"Preserving changed fixture file: {path}")
        path.unlink()

    tree = root / "tree"
    verify_directory(tree)
    linked = tree / "bulk" / "linked-file"
    verify_directory(linked.parent)
    if linked.exists():
        if not linked.samefile(location(root, selected, 0)):
            raise ValueError("Preserving replaced fixture hardlink")
        remove_file(linked, content(0))

    def batch(job):
        first, end = job
        folder = location(root, selected, first).parent
        verify_directory(folder)
        if folder.exists():
            for i in range(first, end):
                remove_file(location(root, selected, i), content(i))
            folder.rmdir()  # An unexpected user file prevents removal.
        return folder.parent

    jobs = [(i, min(end, i + 128)) for begin, end in ((0, selected), (selected, total))
            for i in range(begin, end, 128)]
    with ThreadPoolExecutor(max_workers=8) as pool:
        buckets = set(pool.map(batch, jobs))
    for bucket in buckets:
        if bucket.exists():
            bucket.rmdir()
    edits = tree / "changes"
    verify_directory(edits)
    for i in range(state["edit_count"]):
        remove_file(edits / f"before-{i:04}", content(i))
    for name in ("changes", "empty", "selected", "bulk"):
        folder = tree / name
        verify_directory(folder)
        if folder.exists():
            folder.rmdir()
    tree.rmdir()
    (root / MANIFEST).unlink()
    root.rmdir()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--files", type=int, default=2_048)
    parser.add_argument("--remove", action="store_true", help="remove verified fixture files")
    args = parser.parse_args()
    if args.remove:
        remove(args.directory)
    else:
        print(json.dumps(create(args.directory, args.files)))
