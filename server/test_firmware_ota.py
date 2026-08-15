from __future__ import annotations

import hashlib
import struct
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from firmware_ota import (
    FirmwareImageError,
    inspect_mcuboot_image,
    normalize_version,
    version_is_newer,
)


def make_test_image(*, version: tuple[int, int, int, int] = (1, 2, 3, 4)) -> bytes:
    payload = bytes(range(64))
    header_size = 32
    header = struct.pack(
        "<IIHHIIBBHII",
        0x96F3B83D,
        0,
        header_size,
        0,
        len(payload),
        0,
        version[0],
        version[1],
        version[2],
        version[3],
        0,
    )
    digest = hashlib.sha256(header + payload).digest()
    signature = b"\x30\x06\x02\x01\x01\x02\x01\x01"
    entries = (
        struct.pack("<BBH", 0x10, 0, len(digest)) + digest
        + struct.pack("<BBH", 0x22, 0, len(signature)) + signature
    )
    tlv = struct.pack("<HH", 0x6907, 4 + len(entries)) + entries
    return header + payload + tlv


class FirmwareOtaImageTests(unittest.TestCase):
    def test_semver_comparison(self) -> None:
        self.assertEqual(normalize_version("v1.2.3"), "1.2.3+0")
        self.assertTrue(version_is_newer("1.2.3+5", "1.2.3+4"))
        self.assertFalse(version_is_newer("1.2.3+4", "1.2.3+4"))
        with self.assertRaises(FirmwareImageError):
            normalize_version("2026-07-25")

    def test_inspects_signed_mcuboot_image(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "app.bin"
            key = Path(temp) / "public.pem"
            path.write_bytes(make_test_image())
            key.write_text("test", encoding="utf-8")
            with mock.patch("firmware_ota._verify_ecdsa_signature") as verify:
                result = inspect_mcuboot_image(path, key)
            self.assertTrue(result.valid)
            self.assertEqual(result.image_version, "1.2.3+4")
            self.assertEqual(result.signature_type, "ecdsa-p256")
            verify.assert_called_once()

    def test_rejects_corrupt_digest(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "app.bin"
            image = bytearray(make_test_image())
            image[40] ^= 0x80
            path.write_bytes(image)
            with self.assertRaisesRegex(FirmwareImageError, "image_digest_mismatch"):
                inspect_mcuboot_image(path, Path(temp) / "public.pem")

    def test_requires_public_key(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "app.bin"
            path.write_bytes(make_test_image())
            with self.assertRaisesRegex(FirmwareImageError, "public_key_not_configured"):
                inspect_mcuboot_image(path, None)

    def test_rejects_bad_signature(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "app.bin"
            key = Path(temp) / "public.pem"
            path.write_bytes(make_test_image())
            key.write_text("test", encoding="utf-8")
            with mock.patch(
                "firmware_ota._verify_ecdsa_signature",
                side_effect=FirmwareImageError("signature_verification_failed"),
            ):
                with self.assertRaisesRegex(
                    FirmwareImageError,
                    "signature_verification_failed",
                ):
                    inspect_mcuboot_image(path, key)

    def test_rejects_corrupt_or_missing_signature_tlv(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "app.bin"
            key = Path(temp) / "public.pem"
            image = bytearray(make_test_image())
            regular_tlv_offset = 32 + 64
            image[regular_tlv_offset] ^= 0x01
            path.write_bytes(image)
            with self.assertRaisesRegex(FirmwareImageError, "missing_regular_tlv"):
                inspect_mcuboot_image(path, key)

            image = bytearray(make_test_image())
            signature_type_offset = regular_tlv_offset + 4 + 4 + 32
            image[signature_type_offset] = 0x23
            path.write_bytes(image)
            with self.assertRaisesRegex(FirmwareImageError, "missing_ecdsa_p256_tlv"):
                inspect_mcuboot_image(path, key)


if __name__ == "__main__":
    unittest.main()
