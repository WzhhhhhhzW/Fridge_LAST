"""Create a deployable server source archive without secrets or runtime data."""

from __future__ import annotations

import argparse
import zipfile
from pathlib import Path


SERVER_ROOT = Path(__file__).resolve().parent
DEFAULT_OUTPUT = SERVER_ROOT.parent / "fridge-server-ota-20260725.zip"

EXCLUDED_DIRS = {"__pycache__", ".git", "firmware_seed"}
EXCLUDED_SUFFIXES = {".pyc", ".db", ".db-shm", ".db-wal", ".log"}
EXCLUDED_NAMES = {
    "medicine_store.json",
    "fridge-deepseek.env",
    # Historical cloud snapshot; fridge_deepseek_server.py is authoritative.
    "fridge_deepseek_server.remote.py",
}


def include(path: Path) -> bool:
    relative = path.relative_to(SERVER_ROOT)
    if any(part in EXCLUDED_DIRS for part in relative.parts):
        return False
    if path.name in EXCLUDED_NAMES or path.suffix.lower() in EXCLUDED_SUFFIXES:
        return False
    return path.is_file()


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Package the complete deployable fridge server source tree."
    )
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)

    files = [path for path in sorted(SERVER_ROOT.rglob("*")) if include(path)]
    with zipfile.ZipFile(
        output,
        "w",
        compression=zipfile.ZIP_DEFLATED,
        compresslevel=9,
    ) as archive:
        for path in files:
            # Extract directly into one deployment directory. An extra
            # "server/" prefix makes the web/ directory easy to miss.
            archive.write(path, path.relative_to(SERVER_ROOT))

    print(f"Server source archive: {output}")
    print(f"Files: {len(files)}")
    print(f"Archive size: {output.stat().st_size} bytes")


if __name__ == "__main__":
    main()
