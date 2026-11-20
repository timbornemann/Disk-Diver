import base64
from concurrent.futures import ThreadPoolExecutor
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
from urllib.error import HTTPError

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import updater


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        contexts = contextlib.ExitStack()
        self.addCleanup(contexts.close)
        self.tmp = Path(contexts.enter_context(tempfile.TemporaryDirectory()))
        self.key = Ed25519PrivateKey.generate()
        public = self.key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        self.platform = "windows-x64" if os.name == "nt" else "linux-deb-amd64"
        self.cfg = {"repository": "example/disk-diver", "platform": self.platform,
                    "public_key": base64.b64encode(public).decode(), "version": "1.2.3"}
        self.config = self.tmp / "disk-diver-update.json"
        self.config.write_text(json.dumps(self.cfg))
        self.version = "1.10.2"
        self.name = updater.NAMES[self.platform].format(self.version)
        self.package = b"offline release fixture\0with bytes\n"
        self.asset = {"name": self.name, "size": len(self.package), "sha256": hashlib.sha256(self.package).hexdigest()}
        self.manifest = {"schema": 1, "repository": self.cfg["repository"], "version": self.version,
                         "assets": {self.platform: self.asset}}
        self.base = f"https://github.com/example/disk-diver/releases/download/v{self.version}/"
        self.api = "https://api.github.com/repos/example/disk-diver/releases/latest"
        self.latest = {"tag_name": "v" + self.version, "draft": False, "prerelease": False}
        self.urls = {}
        self.requests = []
        self.publish()
        contexts.enter_context(patch.object(updater, "CONFIG", self.config))
        contexts.enter_context(patch.object(updater, "elevated", return_value=False))
        contexts.enter_context(patch.object(updater, "user_cache", return_value=self.tmp / "cache"))
        contexts.enter_context(patch.object(updater, "download", self.transfer))

    def publish(self):
        raw = (json.dumps(self.manifest, indent=2) + "\n").encode()
        self.urls.update({self.api: json.dumps(self.latest).encode(),
                          self.base + updater.MANIFEST: raw, self.base + updater.MANIFEST + ".sig": self.key.sign(raw),
                          self.base + self.name: self.package})

    def transfer(self, url, limit, deadline):
        self.requests.append(url)
        data = self.urls[url]
        yield data[:3]
        yield data[3:]

    def cli(self, *args):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            code = updater.main(list(args))
        lines = output.getvalue().splitlines()
        self.assertEqual(len(lines), 1)
        result = json.loads(lines[0])
        self.assertEqual(code, int(result["status"] == "error"))
        return result

    def test_fetch_verify_and_reuse(self):
        result = self.cli("fetch")
        self.assertEqual(result["status"], "ready", result)
        path = Path(result["path"])
        self.assertTrue(path.is_absolute())
        self.assertEqual(path.read_bytes(), self.package)
        self.assertEqual(self.requests, [self.api, self.base + updater.MANIFEST, self.base + updater.MANIFEST + ".sig",
                                         self.base + self.name])
        self.requests.clear()
        self.assertEqual(self.cli("verify", str(path)), result)
        self.assertEqual(self.requests, [])
        self.assertEqual(self.cli("fetch"), result)
        self.assertNotIn(self.base + self.name, self.requests)

    def test_disabled_current_older_and_privilege_rules(self):
        with patch.object(updater, "elevated", return_value=True):
            self.assertEqual(self.cli("fetch")["status"], "error")
        self.assertEqual(self.requests, [])
        self.config.write_text(json.dumps(dict(self.cfg, public_key="")))
        self.assertEqual(self.cli("fetch"), {"status": "disabled"})
        self.assertEqual(self.cli("verify", str(self.tmp / "anything"))["status"], "error")
        self.assertEqual(self.requests, [])
        self.config.write_text(json.dumps(self.cfg))
        for version in ("1.2.3", "0.9.0"):
            self.latest["tag_name"] = "v" + version
            self.publish()
            self.assertEqual(self.cli("fetch"), {"status": "current", "version": "1.2.3"})
        self.assertFalse((self.tmp / "cache").exists())

    def test_unstable_or_invalid_release_is_not_downloaded(self):
        for change in ({"prerelease": True}, {"draft": True}, {"tag_name": "v2.0.0-rc1"},
                       {"tag_name": "v65536.0.0"}, {"tag_name": "v02.0.0"}):
            self.requests.clear()
            self.urls[self.api] = json.dumps(dict(self.latest, **change)).encode()
            self.assertEqual(self.cli("fetch")["status"], "error")
            self.assertEqual(self.requests, [self.api])

    def test_rejects_bad_signature_and_unbound_signed_metadata(self):
        self.urls[self.base + updater.MANIFEST] += b" "
        self.assertEqual(self.cli("fetch")["status"], "error")
        self.assertNotIn(self.base + self.name, self.requests)
        for change in ({"schema": True}, {"repository": "other/project"}, {"version": "1.10.1"}, {"assets": {}},
                       {"assets": {self.platform: dict(self.asset, name="../package.exe")}},
                       {"assets": {self.platform: dict(self.asset, size=updater.PACKAGE_LIMIT + 1)}}):
            original = self.manifest
            self.manifest = dict(original, **change)
            self.publish()
            self.assertEqual(self.cli("fetch")["status"], "error")
            self.manifest = original
        self.assertNotIn(self.base + self.name, self.requests)

    def test_incomplete_and_bad_payload_are_never_ready(self):
        for payload in (self.package[:5], self.package + b"x", b"9" * len(self.package)):
            self.urls[self.base + self.name] = payload
            self.assertEqual(self.cli("fetch")["status"], "error")
            self.assertFalse(list(self.tmp.rglob(self.name)))
            self.assertFalse(list(self.tmp.rglob("*.part")))
        self.publish()
        self.assertEqual(self.cli("fetch")["status"], "ready")

    def test_cancel_discards_unpublished_file_then_retry_succeeds(self):
        def interrupted(url, limit, deadline):
            if url == self.base + self.name:
                yield self.package[:3]
                raise KeyboardInterrupt
            yield from self.transfer(url, limit, deadline)
        with patch.object(updater, "download", interrupted), self.assertRaises(KeyboardInterrupt):
            updater.fetch(self.cfg)
        self.assertFalse(list(self.tmp.rglob("*.part")))
        self.assertFalse(list(self.tmp.rglob(self.name)))
        # A previous process kill may leave one private scratch file. It is never
        # accepted by verify and the kernel releases its advisory lock on exit.
        scratch = updater.cache_directory(self.cfg, False) / ".download.part"
        scratch.write_bytes(b"aborted")
        self.assertEqual(self.cli("verify", str(scratch))["status"], "error")
        self.assertEqual(self.cli("fetch")["status"], "ready")
        self.assertFalse(scratch.exists())

    def test_offline_recheck_detects_tamper_and_installed_version(self):
        path = Path(self.cli("fetch")["path"])
        path.write_bytes(b"x" * len(self.package))
        self.assertEqual(self.cli("verify", str(path))["status"], "error")
        self.assertEqual(self.cli("fetch")["status"], "ready")
        (path.parent / (updater.MANIFEST + ".sig")).write_bytes(b"x" * 64)
        self.assertEqual(self.cli("verify", str(path))["status"], "error")
        self.assertEqual(self.cli("fetch")["status"], "ready")
        self.config.write_text(json.dumps(dict(self.cfg, version=self.version)))
        self.assertEqual(self.cli("verify", str(path))["status"], "error")

    def test_offline_recheck_refuses_arbitrary_paths_or_links(self):
        path = Path(self.cli("fetch")["path"])
        outside = self.tmp / self.name
        os.link(path, outside)
        self.assertEqual(self.cli("verify", str(outside))["status"], "error")
        self.assertEqual(self.cli("verify", str(path))["status"], "error")
        self.assertEqual(self.cli("verify", str(path.parent / ".." / path.parent.name / path.name))["status"], "error")

    def test_prune_on_current_only_removes_authenticated_obsolete_release(self):
        package = Path(self.cli("fetch")["path"])
        directory = package.parent
        other = directory.parent / "user-file"
        other.write_text("untouched")
        unknown = directory / "user-notes"
        unknown.write_text("untouched")
        scratch = directory.parent / ".download.part"
        scratch.write_bytes(b"interrupted")
        self.config.write_text(json.dumps(dict(self.cfg, version=self.version)))
        self.assertEqual(self.cli("fetch")["status"], "current")
        self.assertTrue(package.exists())
        self.assertFalse(scratch.exists())
        self.assertEqual(unknown.read_text(), "untouched")
        unknown.unlink()
        linked = self.tmp / "package-link"
        os.link(package, linked)
        self.assertEqual(self.cli("fetch")["status"], "current")
        self.assertTrue(package.exists())
        linked.unlink()
        package.write_bytes(b"x" * len(self.package))
        self.assertEqual(self.cli("fetch")["status"], "current")
        self.assertTrue(package.exists())
        package.write_bytes(self.package)
        self.assertEqual(self.cli("fetch")["status"], "current")
        self.assertFalse(directory.exists())
        self.assertEqual(other.read_text(), "untouched")

    def test_concurrent_fetch_publishes_only_one_complete_package(self):
        gate = threading.Barrier(2)
        def concurrent(url, limit, deadline):
            if url == self.api:
                gate.wait(timeout=5)
            yield from self.transfer(url, limit, deadline)
        with patch.object(updater, "download", concurrent), ThreadPoolExecutor(max_workers=2) as pool:
            results = [pool.submit(updater.fetch, self.cfg) for _ in range(2)]
            a, b = (f.result(timeout=10) for f in results)
        self.assertEqual(a, b)
        self.assertEqual(self.requests.count(self.base + self.name), 1)


class TransportTests(unittest.TestCase):
    def test_redirects_reject_credentials_downgrades_and_other_hosts(self):
        initial = "https://github.com/example/disk-diver/releases/download/v1.0.0/file"
        for url in ("http://github.com/x", "https://github.com.evil.invalid/x", "https://me@github.com/x",
                    "https://objects.githubusercontent.com:444/x", "file:///tmp/file"):
            with self.subTest(url=url), patch.object(updater, "build_opener") as build:
                build.return_value.open.side_effect = HTTPError(initial, 302, "", {"Location": url}, io.BytesIO())
                with self.assertRaises(updater.UpdateError):
                    list(updater.download(initial, 32, time.monotonic() + 5))
                self.assertEqual(build.return_value.open.call_count, 1)

    def test_redirect_delivery_and_stream_limits(self):
        class Response(io.BytesIO):
            status = 200
            headers = {}
        initial = "https://github.com/example/disk-diver/releases/download/v1.0.0/file"
        location = "https://release-assets.githubusercontent.com/release/test"
        with patch.object(updater, "build_opener") as build:
            build.return_value.open.side_effect = [HTTPError(initial, 302, "", {"Location": location}, io.BytesIO()),
                                                   Response(b"data")]
            self.assertEqual(b"".join(updater.download(initial, 4, time.monotonic() + 5)), b"data")
        for data, headers in ((b"long", {}), (b"x", {"Content-Length": "3"}), (b"x", {"Content-Encoding": "gzip"})):
            with patch.object(updater, "build_opener") as build:
                resp = Response(data)
                resp.headers = headers
                build.return_value.open.return_value = resp
                with self.assertRaises(updater.UpdateError):
                    list(updater.download(initial, 3, time.monotonic() + 5))


if __name__ == "__main__":
    unittest.main()
