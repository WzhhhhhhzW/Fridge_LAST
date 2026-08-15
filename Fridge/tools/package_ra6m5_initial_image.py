"""Build one RFP-ready S-record for the RA6M5 dual-bank first flash.

The runtime dual-bank alias of the inactive bank is 0x00200000, but an
external programmer writes the same physical bank through the linear
0x00080000 address range on this 1 MiB RA6M5.  This tool follows the Renesas
MCUboot dual-bank programming layout:

    0x00000000  Bank 0 MCUboot
    0x00020000  Bank 0 signed application
    0x00080000  Bank 1 MCUboot copy

Option-setting records from the original MCUboot S-record are retained once.
The signing private key is neither read nor copied by this tool.
"""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path


BANK_BYTES = 0x80000
BOOT_BYTES = 0x20000
APP_SLOT_BYTES = 0x60000
PRIMARY_APP_ADDRESS = 0x20000
SECONDARY_BOOT_ADDRESS = 0x80000
SECONDARY_APP_ADDRESS = 0xA0000
MCUBOOT_MAGIC = 0x96F3B83D
MCUBOOT_HEADER_BYTES = 0x200


class ImagePackagingError(ValueError):
    """Raised when an input cannot safely be placed in the flash layout."""


def _decode_srec(path: Path) -> dict[int, int]:
    memory: dict[int, int] = {}
    address_bytes_by_type = {"1": 2, "2": 3, "3": 4}

    for line_number, raw_line in enumerate(
        path.read_text(encoding="ascii").splitlines(), start=1
    ):
        line = raw_line.strip()
        if not line:
            continue
        if len(line) < 4 or line[0] != "S":
            raise ImagePackagingError(f"{path}:{line_number}: invalid S-record")

        record_type = line[1]
        if record_type not in address_bytes_by_type:
            continue

        try:
            record = bytes.fromhex(line[2:])
        except ValueError as exc:
            raise ImagePackagingError(
                f"{path}:{line_number}: non-hex S-record"
            ) from exc
        if not record or record[0] != len(record) - 1:
            raise ImagePackagingError(
                f"{path}:{line_number}: invalid S-record byte count"
            )
        if (sum(record) & 0xFF) != 0xFF:
            raise ImagePackagingError(
                f"{path}:{line_number}: invalid S-record checksum"
            )

        address_bytes = address_bytes_by_type[record_type]
        address = int.from_bytes(record[1 : 1 + address_bytes], "big")
        data = record[1 + address_bytes : -1]
        for offset, value in enumerate(data):
            target = address + offset
            previous = memory.get(target)
            if previous is not None and previous != value:
                raise ImagePackagingError(
                    f"{path}:{line_number}: conflicting byte at 0x{target:08X}"
                )
            memory[target] = value

    if not memory:
        raise ImagePackagingError(f"{path}: no S-record data")
    return memory


def _insert(
    destination: dict[int, int],
    address: int,
    data: bytes,
    label: str,
) -> None:
    for offset, value in enumerate(data):
        target = address + offset
        previous = destination.get(target)
        if previous is not None and previous != value:
            raise ImagePackagingError(
                f"{label} overlaps different data at 0x{target:08X}"
            )
        destination[target] = value


def _insert_mapping(
    destination: dict[int, int],
    source: dict[int, int],
    offset: int,
    label: str,
) -> None:
    for source_address, value in source.items():
        target = source_address + offset
        previous = destination.get(target)
        if previous is not None and previous != value:
            raise ImagePackagingError(
                f"{label} overlaps different data at 0x{target:08X}"
            )
        destination[target] = value


def _srec_line(record_type: str, address: int, data: bytes) -> str:
    address_bytes_by_type = {"0": 2, "3": 4, "7": 4}
    address_bytes = address.to_bytes(address_bytes_by_type[record_type], "big")
    count = len(address_bytes) + len(data) + 1
    checksum = (~(count + sum(address_bytes) + sum(data))) & 0xFF
    return f"S{record_type}{count:02X}{address_bytes.hex().upper()}{data.hex().upper()}{checksum:02X}"


def _write_srec(path: Path, memory: dict[int, int]) -> None:
    lines = [_srec_line("0", 0, b"RA6M5_DUAL_BANK")]
    addresses = sorted(memory)
    index = 0

    while index < len(addresses):
        start = addresses[index]
        data = bytearray([memory[start]])
        index += 1
        while (
            index < len(addresses)
            and addresses[index] == start + len(data)
            and len(data) < 32
        ):
            data.append(memory[addresses[index]])
            index += 1
        lines.append(_srec_line("3", start, bytes(data)))

    lines.append(_srec_line("7", 0, b""))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="ascii", newline="\n")


def _validate_application(path: Path) -> tuple[bytes, str]:
    image = path.read_bytes()
    if len(image) != APP_SLOT_BYTES:
        raise ImagePackagingError(
            f"{path}: signed image must be exactly 0x{APP_SLOT_BYTES:X} bytes"
        )
    if len(image) < MCUBOOT_HEADER_BYTES:
        raise ImagePackagingError(f"{path}: truncated MCUboot image")

    magic, _load_address, header_size = struct.unpack_from("<IIH", image, 0)
    if magic != MCUBOOT_MAGIC:
        raise ImagePackagingError(f"{path}: MCUboot magic is missing")
    if header_size != MCUBOOT_HEADER_BYTES:
        raise ImagePackagingError(
            f"{path}: expected 0x{MCUBOOT_HEADER_BYTES:X}-byte MCUboot header"
        )

    major, minor, revision, build = struct.unpack_from("<BBHI", image, 20)
    version = f"{major}.{minor}.{revision}+{build}"
    return image, version


def package(
    bootloader_srec: Path,
    primary_signed: Path,
    output_srec: Path,
    secondary_signed: Path | None,
) -> None:
    bootloader = _decode_srec(bootloader_srec)
    boot_code = {
        address: value
        for address, value in bootloader.items()
        if 0 <= address < BANK_BYTES
    }
    option_records = {
        address: value
        for address, value in bootloader.items()
        if not (0 <= address < BANK_BYTES)
    }
    if not boot_code:
        raise ImagePackagingError("bootloader has no Bank 0 code")
    if max(boot_code) >= BOOT_BYTES:
        raise ImagePackagingError(
            "bootloader exceeds the reserved 0x20000-byte boot region"
        )

    primary_image, primary_version = _validate_application(primary_signed)
    combined: dict[int, int] = {}
    _insert_mapping(combined, boot_code, 0, "Bank 0 MCUboot")
    _insert(combined, PRIMARY_APP_ADDRESS, primary_image, "Bank 0 application")
    _insert_mapping(
        combined,
        boot_code,
        SECONDARY_BOOT_ADDRESS,
        "Bank 1 MCUboot",
    )
    _insert_mapping(combined, option_records, 0, "option-setting records")

    secondary_version = "not populated"
    if secondary_signed is not None:
        secondary_image, secondary_version = _validate_application(secondary_signed)
        _insert(
            combined,
            SECONDARY_APP_ADDRESS,
            secondary_image,
            "Bank 1 application",
        )

    _write_srec(output_srec, combined)
    print(f"output={output_srec.resolve()}")
    print(f"primary_version={primary_version}")
    print(f"secondary_version={secondary_version}")
    print(f"records_bytes={len(combined)}")
    print(f"sha256={hashlib.sha256(output_srec.read_bytes()).hexdigest()}")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Package the first-flash RA6M5 dual-bank MCUboot S-record."
    )
    parser.add_argument("--bootloader", required=True, type=Path)
    parser.add_argument("--primary", required=True, type=Path)
    parser.add_argument("--secondary", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    package(
        args.bootloader.resolve(),
        args.primary.resolve(),
        args.output.resolve(),
        args.secondary.resolve() if args.secondary else None,
    )


if __name__ == "__main__":
    main()
