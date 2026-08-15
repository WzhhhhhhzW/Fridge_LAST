"""Import a prepared firmware seed into the production dashboard database."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
from pathlib import Path

from dashboard import DashboardStore


def digest(path: Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="导入智慧药品冷藏系统首批固件")
    parser.add_argument("--seed", type=Path, default=Path(__file__).with_name("firmware_seed"))
    parser.add_argument("--db", type=Path, default=Path("/opt/fridge-deepseek/dashboard.db"))
    parser.add_argument("--firmware-root", type=Path, default=Path("/opt/fridge-deepseek/firmware"))
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    manifest = json.loads((args.seed / "manifest.json").read_text(encoding="utf-8"))
    args.firmware_root.mkdir(parents=True, exist_ok=True)
    store = DashboardStore(args.db, model="firmware-import", tcp_port=10086, http_port=10087)
    imported = 0
    try:
        for item in manifest.get("releases", []):
            existing = next((release for release in store.firmware_releases(include_unpublished=True)
                             if release["component"] == item["component"] and release["version"] == item["version"]), None)
            release = existing or store.create_firmware_release(
                item["component"], item["version"], item["title"], item.get("changelog", ""),
                channel=item.get("channel", "stable"), published=bool(item.get("published")),
                is_current=bool(item.get("is_current")),
            )
            if existing:
                release = store.update_firmware_release(release["id"], {
                    "title": item["title"], "changelog": item.get("changelog", ""),
                    "channel": item.get("channel", "stable"), "published": bool(item.get("published")),
                    "is_current": bool(item.get("is_current")),
                }) or release
            known = {(artifact["kind"], artifact["sha256"]) for artifact in release.get("artifacts", [])}
            for artifact in item.get("artifacts", []):
                source = args.seed / artifact["file"]
                sha256 = digest(source)
                if sha256 != artifact["sha256"]:
                    raise ValueError(f"SHA-256校验失败：{source.name}")
                stored_name = f"{sha256}{source.suffix.lower()}"
                destination = args.firmware_root / stored_name
                if not destination.exists():
                    shutil.copy2(source, destination)
                key = (artifact["kind"], sha256)
                if key not in known:
                    store.add_firmware_artifact(
                        release["id"], artifact["kind"], artifact["filename"], stored_name,
                        source.stat().st_size, sha256,
                    )
                    imported += 1
    finally:
        store.close()
    print(f"固件目录已初始化，新增文件记录：{imported}")


if __name__ == "__main__":
    main()
