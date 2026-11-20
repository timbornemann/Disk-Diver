"""Check a public GitHub release, or verify an already cached package offline.

    python -I updater.py fetch
    python -I updater.py verify ABSOLUTE_PACKAGE_PATH

Stdout is a single JSON object. A ready result contains version and path. Pass
that path through verify immediately before handing it to the OS installer.
The installed disk-diver-update.json supplies repository, public_key (raw
32-byte Ed25519 key in Base64), version and platform. An empty release channel
disables network access. The client neither installs software nor requests
elevation. Only the signed manifest determines the accepted bytes.
"""

import base64
import contextlib
import hashlib
import http.client
from itertools import islice
import json
import os
from pathlib import Path
import platform
import re
import stat
import sys
import time
from urllib.error import HTTPError
from urllib.parse import urljoin, urlsplit
from urllib.request import build_opener, HTTPRedirectHandler, Request


CONFIG = Path(__file__).resolve().with_name("disk-diver-update.json")
MANIFEST = "disk-diver-update.json"
PACKAGE_LIMIT = 256 * 1024 * 1024
MANIFEST_LIMIT = 64 * 1024
BLOCK = 256 * 1024
VERSION = re.compile(r"(?:0|[1-9][0-9]{0,4})(?:\.(?:0|[1-9][0-9]{0,4})){2}", re.ASCII)
SHA256 = re.compile(r"[0-9a-f]{64}", re.ASCII)
NAMES = {
    "windows-x64": "Disk-Diver-{}-windows-x64.exe",
    "linux-deb-amd64": "disk-diver_{}_amd64.deb",
    "linux-rpm-x86_64": "disk-diver-{}-1.x86_64.rpm",
}
DELIVERY = {"github.com", "release-assets.githubusercontent.com", "objects.githubusercontent.com",
            "github-releases.githubusercontent.com"}


class UpdateError(Exception):
    pass


def check(condition, message):
    if not condition:
        raise UpdateError(message)


def parse_object(data):
    def unique(pairs):
        result = dict(pairs)
        check(len(result) == len(pairs), "Duplicate field in release information")
        return result
    value = json.loads(data.decode("utf-8"), object_pairs_hook=unique)
    check(isinstance(value, dict), "Expected a JSON object")
    return value


def version_tuple(value):
    check(isinstance(value, str) and VERSION.fullmatch(value), "Invalid stable release version")
    parts = tuple(map(int, value.split(".")))
    check(max(parts) <= 65535, "Unsupported stable release version")
    return parts


def configuration():
    if not CONFIG.exists():
        return None
    with CONFIG.open("rb") as stream:
        raw = stream.read(MANIFEST_LIMIT + 1)
    check(len(raw) <= MANIFEST_LIMIT, "Update configuration is too large")
    cfg = parse_object(raw)
    for field in ("repository", "public_key", "platform", "version"):
        check(isinstance(cfg.get(field), str), "Invalid installed update configuration")
    if not all(cfg[k] for k in ("repository", "public_key", "platform")):
        return None
    repo = cfg["repository"]
    check(re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9-]{0,38}/[A-Za-z0-9_][A-Za-z0-9_.-]{0,99}", repo),
          "Invalid GitHub repository")
    version_tuple(cfg["version"])
    key = base64.b64decode(cfg["public_key"], validate=True)
    check(len(key) == 32, "Invalid Ed25519 public key")
    target = cfg["platform"]
    check(target in NAMES and ((os.name == "nt" and target.startswith("windows-")) or
                               (sys.platform.startswith("linux") and target.startswith("linux-"))) and
          platform.machine().lower() in ("x86_64", "amd64"), "Update configuration is for a different platform")
    return cfg


def elevated():
    if os.name != "nt":
        return os.geteuid() == 0
    import ctypes
    return bool(ctypes.windll.shell32.IsUserAnAdmin())


def user_cache():
    if os.name != "nt":
        base = Path(os.environ.get("XDG_CACHE_HOME") or (Path.home() / ".cache"))
        check(base.is_absolute(), "User cache path must be absolute")
        return base
    import ctypes
    buffer = ctypes.create_unicode_buffer(32768)
    check(ctypes.windll.shell32.SHGetFolderPathW(None, 0x1C, None, 0, buffer) == 0, "Cannot locate Local AppData")
    return Path(buffer.value)


def reparse(info):
    return bool(getattr(info, "st_file_attributes", 0) & 0x400)


def private_directory(path, create):
    if create:
        path.mkdir(mode=0o700, exist_ok=True)
    info = path.lstat()
    check(stat.S_ISDIR(info.st_mode) and not reparse(info), "Unsafe update cache directory")
    if os.name != "nt":
        check(info.st_uid == os.getuid() and not info.st_mode & 0o077, "Update cache must be private to this user")


def cache_directory(cfg, create):
    base = user_cache()
    if create:
        base.mkdir(mode=0o700, parents=True, exist_ok=True)
    namespace = hashlib.sha256("\0".join(cfg[k] for k in ("repository", "platform", "public_key")).encode()).hexdigest()
    path = base
    for component in ("disk-diver", "updates", namespace):
        path = path / component
        private_directory(path, create)
    return path


def safe_url(url, hosts):
    parts = urlsplit(url)
    check(parts.scheme == "https" and parts.hostname in hosts and parts.port in (443, None)
          and parts.username is None and parts.password is None and not parts.fragment
          and "\\" not in url and not any(ord(c) < 32 for c in url), "Unexpected GitHub delivery URL")


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def remaining(deadline):
    left = deadline - time.monotonic()
    check(left > 0, "Update check timed out")
    return left


def download(url, limit, deadline):
    """Yield bounded chunks. Close redirect responses without reading their bodies."""
    hosts = {"api.github.com"} if urlsplit(url).hostname == "api.github.com" else DELIVERY
    opener = build_opener(NoRedirect())
    for _ in range(5):
        safe_url(url, hosts)
        req = Request(url, headers={"User-Agent": "Disk-Diver-Updater", "Accept-Encoding": "identity"})
        try:
            response = opener.open(req, timeout=min(15, remaining(deadline)))
        except HTTPError as exc:
            with exc:
                if exc.code in (301, 302, 303, 307, 308) and exc.headers.get("Location"):
                    url = urljoin(url, exc.headers["Location"])
                    continue
                raise UpdateError(f"GitHub returned HTTP {exc.code}") from exc
        with response:
            check(response.status == 200 and response.headers.get("Content-Encoding", "identity") == "identity",
                  "Unexpected release response")
            length = response.headers.get("Content-Length")
            if length is not None:
                check(length.isascii() and length.isdigit() and len(length) <= 12 and int(length) <= limit,
                      "Release response exceeds its expected size")
            received = 0
            while True:
                remaining(deadline)
                block = response.read1(min(BLOCK, limit - received + 1))
                if not block:
                    break
                received += len(block)
                check(received <= limit, "Release response exceeds its expected size")
                yield block
            check(length is None or received == int(length), "Incomplete release response")
        return
    raise UpdateError("Too many GitHub redirects")


def signed_asset(cfg, version, raw, signature):
    """Verify a candidate for installation against the locally configured release channel."""
    check(version_tuple(version) > version_tuple(cfg["version"]), "Signed release must be newer than the installation")
    return _manifest_asset(cfg, version, raw, signature)


def _manifest_asset(cfg, version, raw, signature):
    from cryptography.exceptions import InvalidSignature
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
    check(len(raw) <= MANIFEST_LIMIT and len(signature) == 64, "Invalid signed manifest size")
    key = Ed25519PublicKey.from_public_bytes(base64.b64decode(cfg["public_key"], validate=True))
    try:
        key.verify(signature, raw)
    except InvalidSignature as exc:
        raise UpdateError("Invalid release signature") from exc
    manifest = parse_object(raw)
    check(type(manifest.get("schema")) is int and manifest["schema"] == 1
          and manifest.get("repository") == cfg["repository"] and manifest.get("version") == version,
          "Signed release does not match this repository and version")
    artifacts = manifest.get("assets")
    asset = artifacts.get(cfg["platform"]) if isinstance(artifacts, dict) else None
    check(isinstance(asset, dict) and asset.get("name") == NAMES[cfg["platform"]].format(version),
          "Signed release lacks the expected platform package")
    check(type(asset.get("size")) is int and 0 < asset["size"] <= PACKAGE_LIMIT
          and isinstance(asset.get("sha256"), str) and SHA256.fullmatch(asset["sha256"]),
          "Invalid signed package size or digest")
    return asset


@contextlib.contextmanager
def regular_file(path):
    before = path.lstat()
    check(stat.S_ISREG(before.st_mode) and not reparse(before) and before.st_nlink == 1, "Unsafe update cache file")
    fd = os.open(path, os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0))
    with os.fdopen(fd, "rb") as stream:
        check(os.path.samestat(before, os.fstat(fd)), "Update cache file changed")
        yield stream
        after = path.lstat()
        check(os.path.samestat(before, after) and before.st_mtime_ns == after.st_mtime_ns
              and before.st_size == after.st_size, "Update cache file changed")


def bounded_file(path, limit):
    with regular_file(path) as stream:
        data = stream.read(limit + 1)
    check(len(data) <= limit, "Cached release metadata is too large")
    return data


def verify(cfg, package):
    root = cache_directory(cfg, False)
    package = Path(package)
    check(package.is_absolute() and package.parent.parent == root, "Package is outside the private update cache")
    version, _, digest = package.parent.name.partition("-")
    check(version_tuple(version) > version_tuple(cfg["version"]), "Package version must be newer than the installation")
    check(SHA256.fullmatch(digest) and package.name == NAMES[cfg["platform"]].format(version),
          "Unexpected package cache path")
    check_cached_generation(cfg, package, version, digest)
    return {"status": "ready", "version": version, "path": str(package)}


def check_cached_generation(cfg, package, version, digest):
    private_directory(package.parent, False)
    raw = bounded_file(package.parent / MANIFEST, MANIFEST_LIMIT)
    signature = bounded_file(package.parent / (MANIFEST + ".sig"), 64)
    check(hashlib.sha256(raw).hexdigest() == digest, "Cached manifest does not match its generation")
    asset = _manifest_asset(cfg, version, raw, signature)
    content = hashlib.sha256()
    with regular_file(package) as stream:
        check(os.fstat(stream.fileno()).st_size == asset["size"], "Incorrect cached package size")
        length = 0
        while block := stream.read(min(BLOCK, asset["size"] - length + 1)):
            length += len(block)
            check(length <= asset["size"], "Cached package grew during verification")
            content.update(block)
    check(length == asset["size"] and content.hexdigest() == asset["sha256"], "Incorrect cached package digest")


@contextlib.contextmanager
def write_lock(path, deadline, create=True):
    """A crashed writer releases the OS lock; the next writer can discard its scratch file."""
    flags = os.O_RDWR | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0)
    if create:
        flags |= os.O_CREAT
    fd = os.open(path, flags, 0o600)
    try:
        info = os.fstat(fd)
        check(stat.S_ISREG(info.st_mode) and info.st_nlink == 1 and not reparse(path.lstat()), "Unsafe update lock")
        if os.name == "nt":
            import msvcrt
            def acquire():
                msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            def acquire():
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        while True:
            try:
                acquire()
                break
            except (PermissionError, BlockingIOError):
                time.sleep(min(0.1, remaining(deadline)))
        yield
    finally:
        os.close(fd)


def prune_installed(cfg, deadline):
    """Remove at most two authenticated, already installed generations per check."""
    try:
        root = cache_directory(cfg, False)
        with write_lock(root / ".lock", min(deadline, time.monotonic() + 0.2), create=False), os.scandir(root) as entries:
            scratch = root / ".download.part"
            try:
                info = scratch.lstat()
            except FileNotFoundError:
                info = None
            if info and stat.S_ISREG(info.st_mode) and not reparse(info) and info.st_nlink == 1:
                scratch.unlink()
            installed = version_tuple(cfg["version"])
            candidates = 0
            for entry in islice(entries, 128):
                version, _, digest = entry.name.partition("-")
                if not VERSION.fullmatch(version) or not SHA256.fullmatch(digest):
                    continue
                try:
                    if version_tuple(version) > installed:
                        continue
                    directory = Path(entry.path)
                    private_directory(directory, False)
                    package = directory / NAMES[cfg["platform"]].format(version)
                    with os.scandir(directory) as files:
                        names = {f.name for f in islice(files, 4)}
                    if names != {package.name, MANIFEST, MANIFEST + ".sig"}:
                        continue
                    candidates += 1
                    check_cached_generation(cfg, package, version, digest)
                    # Running Windows installers deny deletion. Keep their proofs intact.
                    package.unlink()
                    (directory / MANIFEST).unlink()
                    (directory / (MANIFEST + ".sig")).unlink()
                    directory.rmdir()
                except (OSError, UpdateError, ValueError, RecursionError):
                    pass
                if candidates >= 2:
                    break
    except (OSError, UpdateError, ImportError):
        pass  # Cache maintenance must not prevent discovery of a new release.


def store_file(scratch, target, chunks, expected=None):
    # Same filesystem as target. Readers only ever see an entire published file.
    flags = os.O_EXCL | os.O_CREAT | os.O_WRONLY | getattr(os, "O_BINARY", 0)
    with os.fdopen(os.open(scratch, flags, 0o600), "wb") as dest:
        digest = hashlib.sha256()
        size = 0
        for block in chunks:
            size += len(block)
            if expected:
                check(size <= expected["size"], "Downloaded package exceeds its signed size")
            dest.write(block)
            digest.update(block)
        if expected:
            check(size == expected["size"] and digest.hexdigest() == expected["sha256"], "Downloaded package failed verification")
        dest.flush()
        os.fsync(dest.fileno())
    os.replace(scratch, target)


def fetch(cfg):
    check(not elevated(), "Download updates from the desktop with normal user privileges")
    deadline = time.monotonic() + 300
    prune_installed(cfg, deadline)
    repo = cfg["repository"]
    latest = parse_object(b"".join(download(f"https://api.github.com/repos/{repo}/releases/latest", 1024 * 1024, deadline)))
    tag = latest.get("tag_name")
    check(latest.get("draft") is False and latest.get("prerelease") is False and isinstance(tag, str) and tag.startswith("v"),
          "GitHub did not return a stable release")
    version = tag[1:]
    if version_tuple(version) <= version_tuple(cfg["version"]):
        return {"status": "current", "version": cfg["version"]}
    base = f"https://github.com/{repo}/releases/download/{tag}/"
    raw = b"".join(download(base + MANIFEST, MANIFEST_LIMIT, deadline))
    signature = b"".join(download(base + MANIFEST + ".sig", 64, deadline))
    asset = signed_asset(cfg, version, raw, signature)
    root = cache_directory(cfg, True)
    generation = root / (version + "-" + hashlib.sha256(raw).hexdigest())
    package = generation / asset["name"]
    with write_lock(root / ".lock", deadline):
        private_directory(generation, True)
        scratch = root / ".download.part"
        scratch.unlink(missing_ok=True)
        try:
            if package.exists():
                try:
                    return verify(cfg, package)
                except (UpdateError, OSError, ValueError):
                    pass  # Repair a missing or damaged cached artifact from the signed release.
            store_file(scratch, generation / MANIFEST, (raw,))
            store_file(scratch, generation / (MANIFEST + ".sig"), (signature,))
            store_file(scratch, package, download(base + asset["name"], asset["size"], deadline), asset)
        finally:
            scratch.unlink(missing_ok=True)
    return {"status": "ready", "version": version, "path": str(package)}


def main(args):
    try:
        check(args == ["fetch"] or len(args) == 2 and args[0] == "verify", "Usage: updater.py fetch | verify PACKAGE")
        cfg = configuration()
        if cfg is None:
            check(args == ["fetch"], "No trusted update channel is configured")
            result = {"status": "disabled"}
        else:
            result = fetch(cfg) if args == ["fetch"] else verify(cfg, args[1])
    except (UpdateError, OSError, ValueError, ImportError, http.client.HTTPException, RecursionError) as exc:
        result = {"status": "error", "message": str(exc) or "Update check failed"}
    print(json.dumps(result, ensure_ascii=True), flush=True)
    return int(result["status"] == "error")


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
