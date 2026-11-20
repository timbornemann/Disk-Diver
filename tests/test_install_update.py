import base64
import hashlib
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import install_update


class InstallStreamTest(unittest.TestCase):
    def setUp(self):
        self.key = Ed25519PrivateKey.generate()
        public = self.key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        self.cfg = dict(repository="example/disk-diver", public_key=base64.b64encode(public).decode(),
                        version="1.0.0", platform="linux-deb-amd64")

    def stream(self, package=b"installer", version="1.1.0"):
        asset = dict(name=f"disk-diver_{version}_amd64.deb", size=9, sha256=hashlib.sha256(b"installer").hexdigest())
        manifest = json.dumps(dict(schema=1, repository=self.cfg["repository"], version=version,
                                   assets={self.cfg["platform"]: asset})).encode()
        return struct.pack("!I", len(manifest)) + self.key.sign(manifest) + manifest + package

    def test_signed_payload_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = install_update.stage_package(self.cfg, io.BytesIO(self.stream()), Path(tmp))
            self.assertEqual(result.name, "disk-diver_1.1.0_amd64.deb")
            self.assertEqual(result.read_bytes(), b"installer")

    def test_invalid_streams_do_not_reach_package_manager(self):
        damaged = bytearray(self.stream())
        damaged[4] ^= 1
        for payload in (self.stream(b""), self.stream(b"installer+"), self.stream(b"Installer"),
                        self.stream(version="1.0.0"), damaged, bytes.fromhex("ffffffff")):
            with self.subTest(), tempfile.TemporaryDirectory() as tmp:
                with self.assertRaises((ValueError, install_update.updater.UpdateError)):
                    install_update.stage_package(self.cfg, io.BytesIO(payload), Path(tmp))


if __name__ == "__main__":
    unittest.main()
