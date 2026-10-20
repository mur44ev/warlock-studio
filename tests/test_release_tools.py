"""Offline safety checks for release flasher packaging; never load a device."""
import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'firmware/aw60'))
import flash_aw60

class ReleaseImageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        image = ROOT/'firmware/aw60/919_v104-aw60-rev9.bin'
        if not image.is_file():
            raise unittest.SkipTest('Build firmware or extract the rev9 release image first')
        cls.original = image.read_bytes()
        cls.manifest = json.loads((ROOT/'firmware/aw60/manifest-rev9.json').read_text())

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.path = Path(self.directory.name)
        self.previous_root = flash_aw60.ROOT
        flash_aw60.ROOT = self.path
        self.write(self.original, self.manifest)

    def tearDown(self):
        flash_aw60.ROOT = self.previous_root
        self.directory.cleanup()

    def write(self, image, manifest):
        (self.path/'919_v104-aw60-rev9.bin').write_bytes(image)
        (self.path/'manifest-rev9.json').write_text(json.dumps(manifest))

    def test_install_does_not_require_stock_and_strips_boot(self):
        self.assertEqual(flash_aw60.image_data(), self.original[65536:])
        self.assertEqual(len(flash_aw60.image_data()), 217376)

    def test_tampered_image_is_rejected(self):
        damaged = bytearray(self.original); damaged[-1] ^= 1
        self.write(damaged, self.manifest)
        with self.assertRaisesRegex(ValueError, 'hash mismatch'):
            flash_aw60.image_data()

    def test_wrong_length_is_rejected_even_with_updated_image_hash(self):
        damaged = self.original[:-1]
        manifest = dict(self.manifest, patched_sha256=hashlib.sha256(damaged).hexdigest())
        self.write(damaged, manifest)
        with self.assertRaisesRegex(ValueError, 'length'):
            flash_aw60.image_data()

    def test_changed_boot_prefix_is_rejected_even_with_updated_image_hash(self):
        damaged = bytearray(self.original); damaged[0] ^= 1
        manifest = dict(self.manifest, patched_sha256=hashlib.sha256(damaged).hexdigest())
        self.write(damaged, manifest)
        with self.assertRaisesRegex(ValueError, 'Boot prefix'):
            flash_aw60.image_data()

if __name__ == '__main__':
    unittest.main()
