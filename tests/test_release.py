import base64
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey


spec = importlib.util.spec_from_file_location("disk_diver_release", Path(__file__).resolve().parents[1] / "packaging/release.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class ReleaseTests(unittest.TestCase):
    repo, version, tag = "example/disk-diver", "1.2.3", "v1.2.3"
    commit = "a" * 40

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.directory = Path(tmp.name)
        key = Ed25519PrivateKey.generate()
        pem = key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption())
        public = key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        env = mock.patch.dict(os.environ, {
            "DISK_DIVER_UPDATE_PRIVATE_KEY": pem.decode(),
            "DISK_DIVER_UPDATE_PUBLIC_KEY": base64.b64encode(public).decode(),
            "GITHUB_EVENT_NAME": "push",
            "RELEASE_SHA": self.commit,
        })
        env.start()
        self.addCleanup(env.stop)
        self.paths = release.package_paths(self.directory, self.version)
        for platform, path in self.paths.items():
            path.parent.mkdir()
            path.write_bytes(release.MAGIC[platform] + platform.encode())
        self.remote = {}
        self.draft = True
        self.uploads = []

    def sign(self):
        release.sign(self.directory, self.repo, self.version)

    def publish(self):
        def gh(args, *, check):
            self.assertEqual(args[:2], ["gh", "release"])
            self.assertNotIn("--clobber", args)
            if args[2] == "upload":
                path = Path(args[4])
                self.assertNotIn(path.name, self.remote)
                self.remote[path.name] = path.read_bytes()
                self.uploads.append(path.name)
            elif args[2] == "download":
                output = Path(args[args.index("--dir") + 1]) / release.SIGNATURE
                output.write_bytes(self.remote[release.SIGNATURE])
            elif args[2] == "edit":
                self.assertTrue(self.draft)
                self.draft = False
            else:
                self.fail(f"Unexpected GitHub mutation: {args}")

        def view(repository, tag):
            self.assertEqual((repository, tag), (self.repo, self.tag))
            return {"isDraft": self.draft, "isImmutable": not self.draft,
                    "assets": [{"name": name, "state": "uploaded", "size": len(data),
                                "digest": "sha256:" + hashlib.sha256(data).hexdigest()}
                               for name, data in self.remote.items()]}

        with mock.patch.object(release, "tag_commit", return_value=self.commit), \
                mock.patch.object(release, "release_view", side_effect=view), \
                mock.patch.object(release.subprocess, "run", side_effect=gh):
            release.publish(self.directory, self.repo, self.tag, self.version)

    def test_manifest_signed_over_exact_bytes_and_bound_to_all_packages(self):
        self.sign()
        payload = (self.directory / release.MANIFEST).read_bytes()
        signature = (self.directory / release.SIGNATURE).read_bytes()
        release.public_key().verify(signature, payload)
        self.assertEqual(len(signature), 64)
        manifest = json.loads(payload)
        self.assertEqual(manifest, {"schema": 1, "repository": self.repo, "version": self.version,
                                    "assets": {key: release.file_metadata(path) for key, path in self.paths.items()}})
        with self.assertRaises(InvalidSignature):
            release.public_key().verify(signature, payload.rstrip())

    def test_missing_or_swapped_package_is_not_signed(self):
        rpm = self.paths["linux-rpm-x86_64"]
        rpm.write_bytes(self.paths["windows-x64"].read_bytes())
        with self.assertRaises(ValueError):
            self.sign()
        rpm.unlink()
        with self.assertRaises(FileNotFoundError):
            self.sign()
        self.assertFalse((self.directory / release.MANIFEST).exists())

    def test_wrong_key_writes_nothing(self):
        os.environ["DISK_DIVER_UPDATE_PUBLIC_KEY"] = base64.b64encode(bytes(32)).decode()
        with self.assertRaises(InvalidSignature):
            self.sign()
        self.assertFalse((self.directory / release.SIGNATURE).exists())

    def test_resume_partial_upload_then_repeat_complete_immutable_release(self):
        self.sign()
        rpm = self.paths["linux-rpm-x86_64"]
        self.remote[rpm.name] = rpm.read_bytes()
        self.publish()
        self.assertFalse(self.draft)
        self.assertNotIn(rpm.name, self.uploads)
        self.assertEqual(self.uploads[-1], release.SIGNATURE)
        self.uploads.clear()
        rpm.write_bytes(release.MAGIC["linux-rpm-x86_64"] + b"another rebuild")
        self.publish()
        self.assertEqual(self.uploads, [])
        self.remote[release.SIGNATURE] = bytes(64)
        with self.assertRaises(InvalidSignature):
            self.publish()

    def test_conflicting_or_locked_partial_release_remains_untouched(self):
        self.sign()
        self.remote[self.paths["linux-deb-amd64"].name] = b"other build"
        with self.assertRaisesRegex(ValueError, "Existing asset differs"):
            self.publish()
        self.assertEqual(self.uploads, [])
        self.draft = False
        with self.assertRaisesRegex(ValueError, "locked"):
            self.publish()
        self.assertEqual(self.uploads, [])

    def test_built_tag_must_still_point_to_original_source(self):
        os.environ["RELEASE_SHA"] = "b" * 40
        with self.assertRaisesRegex(ValueError, "tag changed"):
            self.publish()

    def test_release_tags(self):
        for tag in ("v0.0.0", "v65535.32.7"):
            self.assertEqual(release.release_version(tag), tag[1:])
        for tag in ("v1.0", "1.2.3", "v1.2.3-rc1", "v01.2.3", "v1.1.65536", "v1.1.1\nX", "v1.２.3"):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                release.release_version(tag)


if __name__ == "__main__":
    unittest.main()
