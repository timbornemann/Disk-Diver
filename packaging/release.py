"""Build and publish the signed Disk-Diver update manifest in GitHub Actions."""

import base64
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey


MANIFEST = "disk-diver-update.json"
SIGNATURE = MANIFEST + ".sig"
MAX_PACKAGE_BYTES = 256 * 1024 * 1024
MAGIC = {"windows-x64": b"MZ", "linux-deb-amd64": b"!<arch>\n", "linux-rpm-x86_64": b"\xed\xab\xee\xdb"}


def release_version(tag):
    component = r"(?:0|[1-9][0-9]{0,4})"
    if not re.fullmatch(rf"v{component}\.{component}\.{component}", tag):
        raise ValueError("Expected a stable tag such as v1.2.3")
    version = tag[1:]
    if any(int(part) > 65535 for part in version.split(".")):
        raise ValueError("Release version components must be at most 65535")
    return version


def package_names(version):
    return {
        "windows-x64": f"Disk-Diver-{version}-windows-x64.exe",
        "linux-deb-amd64": f"disk-diver_{version}_amd64.deb",
        "linux-rpm-x86_64": f"disk-diver-{version}-1.x86_64.rpm",
    }


def public_key():
    encoded = os.environ.get("DISK_DIVER_UPDATE_PUBLIC_KEY")
    if not encoded:
        raise ValueError("Set the repository variable DISK_DIVER_UPDATE_PUBLIC_KEY before releasing")
    raw = base64.b64decode(encoded, validate=True)
    return Ed25519PublicKey.from_public_bytes(raw)


def encode_manifest(repository, version, assets):
    manifest = {"schema": 1, "repository": repository, "version": version, "assets": assets}
    return (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8")


def file_metadata(path):
    stat = path.stat()
    if not path.is_file() or path.is_symlink() or not 0 < stat.st_size <= MAX_PACKAGE_BYTES:
        raise ValueError(f"Invalid file or size: {path}")
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return {"name": path.name, "size": stat.st_size, "sha256": digest.hexdigest()}


def package_paths(directory, version):
    return {platform: directory / ("Disk-Diver-" + platform) / name
            for platform, name in package_names(version).items()}


def sign(directory, repository, version):
    assets = {}
    for platform, path in package_paths(directory, version).items():
        assets[platform] = file_metadata(path)
        with path.open("rb") as source:
            if source.read(len(MAGIC[platform])) != MAGIC[platform]:
                raise ValueError(f"Unexpected installer format for {platform}")
    pem = os.environ.get("DISK_DIVER_UPDATE_PRIVATE_KEY")
    if not pem:
        raise ValueError("Set DISK_DIVER_UPDATE_PRIVATE_KEY in the release environment")
    private = serialization.load_pem_private_key(pem.encode(), password=None)
    if not isinstance(private, Ed25519PrivateKey):
        raise ValueError("Expected an Ed25519 PKCS#8 private key")
    payload = encode_manifest(repository, version, assets)
    signature = private.sign(payload)
    # Catch a mismatched environment secret before writing a publishable manifest.
    public_key().verify(signature, payload)
    (directory / MANIFEST).write_bytes(payload)
    (directory / SIGNATURE).write_bytes(signature)


def tag_commit(repository, tag):
    ref = json.loads(subprocess.check_output(["gh", "api", f"repos/{repository}/git/ref/tags/{tag}"]))["object"]
    while ref["type"] == "tag":
        ref = json.loads(subprocess.check_output(["gh", "api", f"repos/{repository}/git/tags/{ref['sha']}"]))["object"]
    if ref["type"] != "commit" or not re.fullmatch("[0-9a-f]{40}", ref["sha"]):
        raise ValueError("Release tag must point to a commit")
    return ref["sha"]


def release_view(repository, tag):
    result = subprocess.run(["gh", "release", "view", tag, "-R", repository,
                             "--json", "tagName,isDraft,isImmutable,isPrerelease,assets"], capture_output=True)
    if result.returncode and result.stderr.strip() == b"release not found":
        return None
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors="replace"))
    release = json.loads(result.stdout)
    if release["tagName"] != tag or release["isPrerelease"]:
        raise ValueError("Expected a stable release with the exact requested tag")
    return release


def indexed_assets(release):
    return {asset["name"]: asset for asset in release["assets"]}


def matching_asset(remote, metadata):
    return (remote["state"] == "uploaded" and remote["size"] == metadata["size"]
            and remote["digest"] == "sha256:" + metadata["sha256"])


def complete_release(repository, version, release):
    """Authenticate a previous run even when a fresh rebuild produces different bytes."""
    names = package_names(version)
    remote = indexed_assets(release)
    if not all(name in remote for name in (*names.values(), MANIFEST, SIGNATURE)):
        return False
    assets = {}
    for platform, name in names.items():
        item = remote[name]
        if (item["state"] != "uploaded" or not 0 < item["size"] <= MAX_PACKAGE_BYTES
                or not re.fullmatch("sha256:[0-9a-f]{64}", item["digest"] or "")):
            raise ValueError(f"Unverified release asset: {name}")
        assets[platform] = {"name": name, "size": item["size"], "sha256": item["digest"][7:]}
    payload = encode_manifest(repository, version, assets)
    if not matching_asset(remote[MANIFEST], {"size": len(payload), "sha256": hashlib.sha256(payload).hexdigest()}):
        raise ValueError("Published manifest differs from the release assets")
    if remote[SIGNATURE]["state"] != "uploaded" or remote[SIGNATURE]["size"] != 64:
        raise ValueError("Published signature must contain exactly 64 bytes")
    with tempfile.TemporaryDirectory() as scratch:
        subprocess.run(["gh", "release", "download", "v" + version, "-R", repository,
                        "--pattern", SIGNATURE, "--dir", scratch], check=True)
        public_key().verify((Path(scratch) / SIGNATURE).read_bytes(), payload)
    return True


def require_mutable(release):
    if release and release["isImmutable"]:
        raise ValueError("This release was locked before all installers were attached. "
                         "Push a new version tag and let the release workflow publish its draft.")


def prepare(repository, tag, version):
    public_key()
    sha = tag_commit(repository, tag)
    if os.environ["GITHUB_EVENT_NAME"] != "workflow_dispatch":
        event_commit = subprocess.check_output(["git", "rev-parse", "HEAD^{commit}"], text=True).strip()
        if sha != event_commit:
            raise ValueError("Release tag has moved since the triggering event")
    release = release_view(repository, tag)
    complete = bool(release and complete_release(repository, version, release))
    if not complete:
        require_mutable(release)
    with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as outputs:
        print(f"sha={sha}\nversion={version}\npublic_key={os.environ['DISK_DIVER_UPDATE_PUBLIC_KEY']}", file=outputs)
        print(f"complete={str(complete and not release['isDraft']).lower()}", file=outputs)


def upload_missing(repository, tag, paths, remote):
    # Preflight every collision before changing a partially uploaded release.
    missing = []
    for path in paths:
        if path.name not in remote:
            missing.append(path)
        elif not matching_asset(remote[path.name], file_metadata(path)):
            raise ValueError(f"Existing asset differs: {path.name}. Re-run the failed publishing job "
                             "with its original build artifacts or use a new version.")
    for path in missing:
        subprocess.run(["gh", "release", "upload", tag, str(path), "-R", repository], check=True)


def publish(directory, repository, tag, version):
    sha = os.environ["RELEASE_SHA"]
    if tag_commit(repository, tag) != sha:
        raise ValueError("Release tag changed during the build")
    release = release_view(repository, tag)
    if release is None:
        if os.environ["GITHUB_EVENT_NAME"] == "release":
            raise ValueError("The published release was removed during the build")
        subprocess.run(["gh", "release", "create", tag, "-R", repository, "--draft", "--verify-tag",
                        "--target", sha, "--generate-notes"], check=True)
        release = release_view(repository, tag)
    if not complete_release(repository, version, release):
        require_mutable(release)
        payload = (directory / MANIFEST).read_bytes()
        public_key().verify((directory / SIGNATURE).read_bytes(), payload)
        paths = package_paths(directory, version)
        local = encode_manifest(repository, version, {k: file_metadata(p) for k, p in paths.items()})
        if local != payload:
            raise ValueError("Signed manifest differs from the local packages")
        # Upload the detached signature last so clients see a complete authenticated set.
        upload_missing(repository, tag, [*paths.values(), directory / MANIFEST, directory / SIGNATURE],
                       indexed_assets(release))
        release = release_view(repository, tag)
        if not complete_release(repository, version, release):
            raise ValueError("Release upload incomplete")
    if tag_commit(repository, tag) != sha:
        raise ValueError("Release tag changed during upload")
    if release["isDraft"]:
        subprocess.run(["gh", "release", "edit", tag, "-R", repository, "--draft=false"], check=True)
    print(f"Verified installers and signed manifest for {repository} {tag}")


def main():
    if sys.argv[1:2] == ["check-installed"]:
        install_dir, platform = sys.argv[2:]
        config = json.loads((Path(install_dir) / MANIFEST).read_bytes())
        expected = {"repository": os.environ["RELEASE_REPO"], "public_key": os.environ["RELEASE_PUBLIC_KEY"],
                    "platform": platform, "version": os.environ["RELEASE_VERSION"] or release_version("v" + config["version"])}
        if config != expected:
            raise ValueError("Installed update channel differs from the build inputs")
        return
    operation, = sys.argv[1:]
    repository = os.environ["GITHUB_REPOSITORY"]
    if not re.fullmatch(r"[A-Za-z0-9-]+/[A-Za-z0-9_.-]+", repository):
        raise ValueError("Invalid GitHub repository")
    tag = os.environ["RELEASE_TAG"]
    version = release_version(tag)
    if operation == "prepare":
        prepare(repository, tag, version)
    elif operation == "sign":
        sign(Path("packages"), repository, version)
    elif operation == "publish":
        publish(Path("packages"), repository, tag, version)
    else:
        raise ValueError("Use prepare, sign or publish")


if __name__ == "__main__":
    main()
