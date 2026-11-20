"""Install a signed Linux update using the system package manager and polkit."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

# Both scripts are installed side by side. -I excludes other user import paths.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import updater


INSTALLED = Path("/usr/lib/disk-diver/install_update.py")
MAX_MANIFEST = 64 * 1024


def read_exact(source, size):
    data = source.read(size)
    if len(data) != size:
        raise ValueError("The update data is incomplete.")
    return data


def stage_package(cfg, source, staging):
    length, = struct.unpack("!I", read_exact(source, 4))
    if not 0 < length <= MAX_MANIFEST:
        raise ValueError("Invalid update manifest size.")
    signature = read_exact(source, 64)
    manifest = read_exact(source, length)
    version = json.loads(manifest)["version"]
    asset = updater.signed_asset(cfg, version, manifest, signature)
    if updater.version_tuple(version) <= updater.version_tuple(cfg["version"]):
        raise ValueError("The package must contain a newer version.")
    path = staging / asset["name"]
    digest = hashlib.sha256()
    remaining = asset["size"]
    with path.open("xb") as package:
        while remaining:
            chunk = read_exact(source, min(remaining, 256 * 1024))
            package.write(chunk)
            digest.update(chunk)
            remaining -= len(chunk)
    if source.read(1) or digest.hexdigest() != asset["sha256"]:
        raise ValueError("Package verification failed.")
    return path


def apply():
    import fcntl

    if os.geteuid() != 0 or Path(__file__).resolve() != INSTALLED:
        raise ValueError("This update requires a system-wide installation of Disk-Diver.")
    program = {"linux-deb-amd64": "/usr/bin/apt-get", "linux-rpm-x86_64": "/usr/bin/dnf"}
    # The authorized process consumes bytes only. It never opens a caller-supplied
    # pathname as root. apt/dnf sees only the independently verified private copy.
    with open("/run/disk-diver-update.lock", "ab") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        cfg = updater.configuration()
        if not cfg or cfg["platform"] not in program:
            raise ValueError("This package has no configured update source.")
        with tempfile.TemporaryDirectory(prefix="disk-diver-update-", dir="/var/tmp") as tmp:
            package = stage_package(cfg, sys.stdin.buffer, Path(tmp))
            subprocess.run([program[cfg["platform"]], "-y", "install", str(package)], cwd="/",
                           stdin=subprocess.DEVNULL, stdout=sys.stderr, check=True,
                           env={**os.environ, "LC_ALL": "C"})


def request_install(path):
    if Path(__file__).resolve() != INSTALLED or os.geteuid() == 0:
        raise ValueError("Start the update from the installed desktop app.")
    with (path.parent / updater.MANIFEST).open("rb") as file:
        manifest = file.read(MAX_MANIFEST + 1)
    if not 0 < len(manifest) <= MAX_MANIFEST:
        raise ValueError("Invalid update manifest size.")
    with (path.parent / (updater.MANIFEST + ".sig")).open("rb") as file:
        signature = file.read(65)
    if len(signature) != 64:
        raise ValueError("Invalid update signature size.")
    with path.open("rb") as package, subprocess.Popen(
        ["/usr/bin/pkexec", "/usr/bin/python3", "-I", "-B", str(INSTALLED), "--apply"],
        cwd="/", stdin=subprocess.PIPE, env={**os.environ, "LC_ALL": "C"},
    ) as process:
        try:
            with process.stdin:
                process.stdin.write(struct.pack("!I", len(manifest)) + signature + manifest)
                shutil.copyfileobj(package, process.stdin, length=256 * 1024)
        except BrokenPipeError:
            pass
        return process.wait()


if __name__ == "__main__":
    try:
        if sys.argv[1:] == ["--apply"]:
            apply()
        elif len(sys.argv) == 2 and Path(sys.argv[1]).is_absolute():
            sys.exit(request_install(Path(sys.argv[1])))
        else:
            raise ValueError("Invalid installation request.")
    except Exception as error:
        print(error, file=sys.stderr)
        sys.exit(1)
