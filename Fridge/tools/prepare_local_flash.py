#!/usr/bin/env python3
"""Build and verify the signed MCUboot image used for local USB flashing."""

from __future__ import annotations

import shutil
import sys
from pathlib import Path

from ota_packager_core import (
    MCUBOOT_SLOT_SIZE,
    PackagerError,
    build_packages,
    inspect_mcuboot_image,
    read_source_version,
)


ROOT = Path(__file__).resolve().parent.parent
PROJECT = ROOT / "project_firdge"
DEBUG = PROJECT / "Debug"
ELF = DEBUG / "project_firdge.elf"
CONFIG = PROJECT / "src" / "ota_update_config.h"
PREFERENCES = PROJECT / ".settings" / "org.eclipse.cdt.core.prefs"
STABLE_IMAGE = DEBUG / "project_firdge.local.confirmed.bin"
MCUBOOT_TRAILER_MAGIC = bytes.fromhex("77c295f360d2ef7f3552500f2cb67980")


def read_eclipse_value(name: str) -> str:
    try:
        lines = PREFERENCES.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise PackagerError(f"Cannot read e2 studio build settings: {PREFERENCES}") from exc

    suffix = f"/{name}/value="
    for line in lines:
        if suffix in line:
            value = line.split("=", 1)[1]
            return value.replace(r"\:", ":").replace(r"\\", "\\")
    raise PackagerError(f"e2 studio build variable {name} is missing")


def is_confirmed_template(path: Path) -> bool:
    try:
        image = path.read_bytes()
    except OSError:
        return False
    return (
        len(image) == MCUBOOT_SLOT_SIZE
        and image[-24] == 1
        and image[-16:] == MCUBOOT_TRAILER_MAGIC
    )


def find_confirmed_template(version: str) -> Path:
    target = DEBUG / f"project_firdge_{version}_signed_keyhash_confirmed.bin"
    patterns = (
        "project_firdge_*_signed_keyhash_confirmed.bin",
        "project_firdge_*_signed_keyhash.bin",
    )
    candidates: list[Path] = []
    for pattern in patterns:
        candidates.extend(DEBUG.glob(pattern))
    candidates = sorted(
        (path for path in set(candidates) if path != target),
        key=lambda path: path.stat().st_mtime,
        reverse=True,
    )
    for candidate in candidates:
        if is_confirmed_template(candidate):
            return candidate
    if target.is_file() and is_confirmed_template(target):
        return target
    raise PackagerError(
        "No confirmed MCUboot template was found in project_firdge/Debug. "
        "Keep a previously working *_signed_keyhash_confirmed.bin image."
    )


def main() -> int:
    try:
        version = read_source_version(CONFIG)
        build_version = read_eclipse_value("MCUBOOT_IMAGE_VERSION")
        if build_version != version:
            raise PackagerError(
                f"Version mismatch: source is {version}, but e2 studio uses {build_version}. "
                "Update MCUBOOT_IMAGE_VERSION and rebuild the project."
            )

        private_key = Path(read_eclipse_value("MCUBOOT_IMAGE_SIGNING_KEY"))
        template = find_confirmed_template(version)
        print(f"[PACK] Source version : {version}")
        print(f"[PACK] ELF            : {ELF}")
        print(f"[PACK] Template       : {template.name}")

        result = build_packages(
            project_dir=PROJECT,
            elf_path=ELF,
            config_path=CONFIG,
            private_key=private_key,
            template_signed_bin=template,
            output_dir=DEBUG,
            version=version,
            log=lambda message: print(f"[PACK] {message}"),
        )
        info = inspect_mcuboot_image(result.local_confirmed_bin, version)
        if not is_confirmed_template(result.local_confirmed_bin):
            raise PackagerError("Generated image does not contain the MCUboot confirmed trailer")

        temporary = STABLE_IMAGE.with_suffix(".bin.tmp")
        shutil.copyfile(result.local_confirmed_bin, temporary)
        temporary.replace(STABLE_IMAGE)
        inspect_mcuboot_image(STABLE_IMAGE, version)

        print(f"[PACK] Version        : {info['version']}")
        print(f"[PACK] Application    : {info['application_bytes']} bytes")
        print(f"[PACK] Slot image     : {STABLE_IMAGE} ({STABLE_IMAGE.stat().st_size} bytes)")
        print("[PACK] Signature/TLV  : verified")
        print("[PACK] Trailer        : confirmed")
        return 0
    except (PackagerError, OSError, ValueError) as exc:
        print(f"[ERROR] {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
