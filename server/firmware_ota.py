"""MCUboot image inspection helpers for RA6M5 OTA deployments."""

from __future__ import annotations

import hashlib
import re
import struct
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any


MCUBOOT_IMAGE_MAGIC = 0x96F3B83D
MCUBOOT_TLV_INFO_MAGIC = 0x6907
MCUBOOT_PROTECTED_TLV_INFO_MAGIC = 0x6908
MCUBOOT_TLV_SHA256 = 0x10
MCUBOOT_TLV_ECDSA256 = 0x22
MCUBOOT_HEADER_SIZE = 32
MAX_MCUBOOT_HEADER_SIZE = 4096
MAX_MCUBOOT_IMAGE_BYTES = 512 * 1024

_VERSION_RE = re.compile(
    r"^(?:v)?(?P<major>0|[1-9]\d*)\."
    r"(?P<minor>0|[1-9]\d*)\."
    r"(?P<revision>0|[1-9]\d*)"
    r"(?:\+(?P<build>0|[1-9]\d*))?$"
)


class FirmwareImageError(ValueError):
    """Raised when an uploaded image is not a deployable MCUboot image."""


@dataclass(frozen=True)
class FirmwareImageInspection:
    image_version: str
    header_size: int
    payload_size: int
    signed_size: int
    sha256: str
    signature_type: str
    valid: bool
    validation_error: str

    def as_dict(self) -> dict[str, Any]:
        return asdict(self)


def parse_version(value: str) -> tuple[int, int, int, int]:
    match = _VERSION_RE.fullmatch(str(value or "").strip())
    if match is None:
        raise FirmwareImageError("version_must_be_semver")
    return (
        int(match.group("major")),
        int(match.group("minor")),
        int(match.group("revision")),
        int(match.group("build") or 0),
    )


def normalize_version(value: str) -> str:
    major, minor, revision, build = parse_version(value)
    return f"{major}.{minor}.{revision}+{build}"


def version_is_newer(candidate: str, current: str) -> bool:
    return parse_version(candidate) > parse_version(current)


def _format_header_version(major: int, minor: int, revision: int, build: int) -> str:
    return f"{major}.{minor}.{revision}+{build}"


def _parse_tlvs(data: bytes, offset: int) -> dict[int, list[bytes]]:
    if offset + 4 > len(data):
        raise FirmwareImageError("missing_tlv_info")
    magic, total = struct.unpack_from("<HH", data, offset)
    if magic != MCUBOOT_TLV_INFO_MAGIC:
        raise FirmwareImageError("missing_regular_tlv")
    if total < 4 or offset + total > len(data):
        raise FirmwareImageError("invalid_tlv_size")

    result: dict[int, list[bytes]] = {}
    cursor = offset + 4
    end = offset + total
    while cursor < end:
        if cursor + 4 > end:
            raise FirmwareImageError("truncated_tlv_header")
        tlv_type, _pad, length = struct.unpack_from("<BBH", data, cursor)
        cursor += 4
        if cursor + length > end:
            raise FirmwareImageError("truncated_tlv_value")
        result.setdefault(tlv_type, []).append(data[cursor:cursor + length])
        cursor += length
    return result


def _verify_ecdsa_signature(public_key_path: Path, digest: bytes, signature: bytes) -> None:
    try:
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import ec, utils
    except ImportError as exc:  # pragma: no cover - production dependency check
        raise FirmwareImageError("cryptography_dependency_missing") from exc

    try:
        public_key = serialization.load_pem_public_key(public_key_path.read_bytes())
    except (OSError, ValueError) as exc:
        raise FirmwareImageError("public_key_unreadable") from exc
    if not isinstance(public_key, ec.EllipticCurvePublicKey):
        raise FirmwareImageError("public_key_is_not_ec")
    if not isinstance(public_key.curve, ec.SECP256R1):
        raise FirmwareImageError("public_key_is_not_p256")
    try:
        public_key.verify(
            signature,
            digest,
            ec.ECDSA(utils.Prehashed(hashes.SHA256())),
        )
    except Exception as exc:
        raise FirmwareImageError("signature_verification_failed") from exc


def inspect_mcuboot_image(path: Path, public_key_path: Path | None) -> FirmwareImageInspection:
    """Validate one signed MCUboot image and return deployment metadata."""
    try:
        data = Path(path).read_bytes()
    except OSError as exc:
        raise FirmwareImageError("image_unreadable") from exc
    if len(data) < MCUBOOT_HEADER_SIZE or len(data) > MAX_MCUBOOT_IMAGE_BYTES:
        raise FirmwareImageError("image_size_out_of_range")

    (
        magic,
        _load_address,
        header_size,
        protected_tlv_size,
        payload_size,
        _flags,
        major,
        minor,
        revision,
        build,
        _pad,
    ) = struct.unpack_from("<IIHHIIBBHII", data, 0)
    if magic != MCUBOOT_IMAGE_MAGIC:
        raise FirmwareImageError("invalid_mcuboot_magic")
    if header_size < MCUBOOT_HEADER_SIZE or header_size > MAX_MCUBOOT_HEADER_SIZE:
        raise FirmwareImageError("invalid_header_size")

    image_end = header_size + payload_size
    regular_tlv_offset = image_end + protected_tlv_size
    if image_end > len(data) or regular_tlv_offset > len(data):
        raise FirmwareImageError("truncated_image")
    if protected_tlv_size:
        if image_end + 4 > len(data):
            raise FirmwareImageError("truncated_protected_tlv")
        protected_magic, protected_total = struct.unpack_from("<HH", data, image_end)
        if (
            protected_magic != MCUBOOT_PROTECTED_TLV_INFO_MAGIC
            or protected_total != protected_tlv_size
        ):
            raise FirmwareImageError("invalid_protected_tlv")

    tlvs = _parse_tlvs(data, regular_tlv_offset)
    digest_values = tlvs.get(MCUBOOT_TLV_SHA256, [])
    signature_values = tlvs.get(MCUBOOT_TLV_ECDSA256, [])
    if len(digest_values) != 1 or len(digest_values[0]) != hashlib.sha256().digest_size:
        raise FirmwareImageError("missing_sha256_tlv")
    if len(signature_values) != 1 or not signature_values[0]:
        raise FirmwareImageError("missing_ecdsa_p256_tlv")

    calculated_digest = hashlib.sha256(data[:regular_tlv_offset]).digest()
    if calculated_digest != digest_values[0]:
        raise FirmwareImageError("image_digest_mismatch")
    if public_key_path is None:
        raise FirmwareImageError("public_key_not_configured")
    _verify_ecdsa_signature(public_key_path, calculated_digest, signature_values[0])

    version = _format_header_version(major, minor, revision, build)
    return FirmwareImageInspection(
        image_version=version,
        header_size=header_size,
        payload_size=payload_size,
        signed_size=len(data),
        sha256=hashlib.sha256(data).hexdigest(),
        signature_type="ecdsa-p256",
        valid=True,
        validation_error="",
    )


def failed_inspection(path: Path, error: Exception) -> FirmwareImageInspection:
    try:
        size = Path(path).stat().st_size
        digest = hashlib.sha256(Path(path).read_bytes()).hexdigest()
    except OSError:
        size = 0
        digest = ""
    return FirmwareImageInspection(
        image_version="",
        header_size=0,
        payload_size=0,
        signed_size=size,
        sha256=digest,
        signature_type="ecdsa-p256",
        valid=False,
        validation_error=str(error)[:120],
    )
