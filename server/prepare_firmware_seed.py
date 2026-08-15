"""Build the first downloadable firmware catalog from the local workspace.

Run on Windows before deployment:
    python server/prepare_firmware_seed.py

The generated ``server/firmware_seed`` directory is safe to upload as a whole.
It contains install images, compact source archives and a manifest.  Build
directories and bundled FSP object files are intentionally excluded from the
source archives.
"""

from __future__ import annotations

import hashlib
import json
import shutil
import zipfile
from pathlib import Path


HERE = Path(__file__).resolve().parent
WORKSPACE = HERE.parent
OUTPUT = HERE / "firmware_seed"
PROJECT_NAME = "project_firdge"

VERSIONS = [
    ("Fridge", "v2026.07.17", "当前主控程序", True),
    ("Fridge_备份4", "v2026.07.16", "历史主控程序 · 备份4", False),
    ("Fridge_备份3", "v2026.07.08", "历史主控程序 · 备份3", False),
    ("Fridge_备份2", "v2026.07.06", "历史主控程序 · 备份2", False),
    ("Fridge_备份", "v2026.07.05", "历史主控程序 · 备份1", False),
]

SOURCE_TOP_FILES = {".project", ".cproject", "configuration.xml"}
SOURCE_DIRS = {"src", "ra_cfg", "ra_gen", "script"}
SOURCE_EXCLUDED_SUFFIXES = {".o", ".d", ".elf", ".srec", ".map", ".zip", ".hmi"}


def digest(path: Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


def compact_source_zip(project_root: Path, destination: Path) -> None:
    with zipfile.ZipFile(destination, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in sorted(project_root.rglob("*")):
            if not path.is_file():
                continue
            relative = path.relative_to(project_root)
            if relative.parts[0] not in SOURCE_DIRS and relative.as_posix() not in SOURCE_TOP_FILES:
                continue
            if path.suffix.lower() in SOURCE_EXCLUDED_SUFFIXES:
                continue
            archive.write(path, Path(PROJECT_NAME) / relative)


def add_artifact(source: Path, staged_name: str, download_name: str, kind: str) -> dict[str, object]:
    destination = OUTPUT / staged_name
    if source.resolve() != destination.resolve():
        shutil.copy2(source, destination)
    return {
        "kind": kind,
        "file": staged_name,
        "filename": download_name,
        "size_bytes": destination.stat().st_size,
        "sha256": digest(destination),
    }


def main() -> None:
    if OUTPUT.exists():
        shutil.rmtree(OUTPUT)
    OUTPUT.mkdir(parents=True)
    releases: list[dict[str, object]] = []

    for folder, version, title, current in VERSIONS:
        root = WORKSPACE / folder
        project = root / PROJECT_NAME
        srec = project / "Debug" / f"{PROJECT_NAME}.srec"
        if not srec.is_file():
            raise FileNotFoundError(f"缺少主控固件：{srec}")
        source_zip = OUTPUT / f"main-{version}-source.zip"
        compact_source_zip(project, source_zip)
        releases.append({
            "component": "main",
            "version": version,
            "title": title,
            "changelog": "RA6M5主控安装固件与精简源码归档。" if current else "历史可回退版本归档。",
            "channel": "stable" if current else "legacy",
            "published": True,
            "is_current": current,
            "artifacts": [
                add_artifact(srec, f"main-{version}.srec", f"fridge-main-{version}.srec", "install"),
                add_artifact(source_zip, source_zip.name, f"fridge-main-{version}-source.zip", "source"),
            ],
        })

    hmi = WORKSPACE / "Fridge" / "RA6M5.HMI"
    if not hmi.is_file():
        raise FileNotFoundError(f"缺少串口屏工程：{hmi}")
    releases.append({
        "component": "hmi",
        "version": "v2026.05.14",
        "title": "当前TJC串口屏程序",
        "changelog": "当前串口屏HMI工程，可用于二次编辑和下载。",
        "channel": "stable",
        "published": True,
        "is_current": True,
        "artifacts": [add_artifact(hmi, "hmi-v2026.05.14.hmi", "fridge-screen-v2026.05.14.HMI", "project")],
    })

    manifest = {"schema": 1, "generated_from": str(WORKSPACE), "releases": releases}
    (OUTPUT / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"固件种子已生成：{OUTPUT}")
    print(f"版本数：{len(releases)}，文件数：{sum(len(item['artifacts']) for item in releases)}")


if __name__ == "__main__":
    main()
