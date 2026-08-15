"""Build, validate, and upload signed RA6M5 MCUboot OTA images."""

from __future__ import annotations

import hashlib
import http.cookiejar
import json
import re
import shutil
import struct
import subprocess
import tempfile
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable


MCUBOOT_MAGIC = 0x96F3B83D
MCUBOOT_TLV_INFO_MAGIC = 0x6907
MCUBOOT_SLOT_SIZE = 0x60000
MCUBOOT_HEADER_SIZE = 0x200
LOWER_SLOT_ADDRESS = 0x00020000
TARGET_PART = "R7FA6M5BF2CBG"
MIN_BOOTLOADER = "1.0.0+0"
VERSION_RE = re.compile(r"^(\d+)\.(\d+)\.(\d+)\+(\d+)$")
SOURCE_VERSION_RE = re.compile(
    r'^\s*#define\s+OTA_UPDATE_APP_VERSION\s+"([^"]+)"', re.MULTILINE
)


class PackagerError(RuntimeError):
    """Stable user-facing packaging or upload failure."""


@dataclass(frozen=True)
class PackageResult:
    version: str
    unsigned_bin: Path
    ota_pending_bin: Path
    local_confirmed_bin: Path
    local_sparse_hex: Path
    application_bytes: int
    sha256: str


def normalize_version(value: str) -> str:
    value = value.strip()
    match = VERSION_RE.fullmatch(value)
    if match is None:
        raise PackagerError("版本号必须采用 major.minor.patch+build，例如 1.0.0+15")
    major, minor, revision, build = (int(part) for part in match.groups())
    if major > 255 or minor > 255 or revision > 65535 or build > 0xFFFFFFFF:
        raise PackagerError("版本号超出 MCUboot 可表示范围")
    return f"{major}.{minor}.{revision}+{build}"


def read_source_version(config_path: Path) -> str:
    try:
        text = config_path.read_text(encoding="utf-8")
    except OSError as exc:
        raise PackagerError(f"无法读取版本配置：{config_path}") from exc
    match = SOURCE_VERSION_RE.search(text)
    if match is None:
        raise PackagerError("ota_update_config.h 中没有找到 OTA_UPDATE_APP_VERSION")
    return normalize_version(match.group(1))


def _find_executable(explicit: str, names: Iterable[str], candidates: Iterable[Path]) -> str:
    if explicit:
        path = Path(explicit).expanduser()
        if path.is_file():
            return str(path.resolve())
        raise PackagerError(f"工具不存在：{path}")
    for name in names:
        found = shutil.which(name)
        if found:
            return found
    for path in candidates:
        if path.is_file():
            return str(path.resolve())
    raise PackagerError(f"没有找到工具：{', '.join(names)}")


def find_node(explicit: str = "") -> str:
    return _find_executable(
        explicit,
        ("node", "node.exe"),
        (
            Path(r"D:\Program Files (x86)\node.exe"),
            Path(r"C:\Program Files\nodejs\node.exe"),
            Path(r"C:\Program Files (x86)\nodejs\node.exe"),
        ),
    )


def find_objcopy(explicit: str = "") -> str:
    return _find_executable(
        explicit,
        ("arm-none-eabi-objcopy", "arm-none-eabi-objcopy.exe"),
        (
            Path(
                r"D:\Program Files (x86)\STM32CubeCLT_1.19.0\GNU-tools-for-STM32\bin"
                r"\arm-none-eabi-objcopy.exe"
            ),
        ),
    )


def _run(command: list[str], log: Callable[[str], None]) -> None:
    display = " ".join(f'"{part}"' if " " in part else part for part in command)
    log(f"> {display}")
    completed = subprocess.run(
        command,
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if completed.stdout.strip():
        log(completed.stdout.strip())
    if completed.stderr.strip():
        log(completed.stderr.strip())
    if completed.returncode != 0:
        raise PackagerError(f"命令执行失败，退出码 {completed.returncode}")


def inspect_mcuboot_image(path: Path, expected_version: str | None = None) -> dict[str, object]:
    try:
        image = path.read_bytes()
    except OSError as exc:
        raise PackagerError(f"无法读取镜像：{path}") from exc
    if len(image) != MCUBOOT_SLOT_SIZE:
        raise PackagerError(f"OTA镜像必须是 {MCUBOOT_SLOT_SIZE} 字节，实际为 {len(image)}")
    magic, = struct.unpack_from("<I", image, 0)
    if magic != MCUBOOT_MAGIC:
        raise PackagerError("MCUboot 镜像头 magic 错误")
    header_size, protected_size = struct.unpack_from("<HH", image, 8)
    image_size, = struct.unpack_from("<I", image, 12)
    major, minor, revision, build = struct.unpack_from("<BBHI", image, 20)
    version = f"{major}.{minor}.{revision}+{build}"
    if header_size != MCUBOOT_HEADER_SIZE:
        raise PackagerError(f"MCUboot header 应为 0x200，实际为 0x{header_size:X}")
    if expected_version is not None and version != normalize_version(expected_version):
        raise PackagerError(f"镜像版本 {version} 与要求版本 {expected_version} 不一致")
    tlv_offset = header_size + image_size + protected_size
    if tlv_offset + 4 > len(image):
        raise PackagerError("MCUboot TLV 超出镜像范围")
    tlv_magic, tlv_total = struct.unpack_from("<HH", image, tlv_offset)
    if tlv_magic != MCUBOOT_TLV_INFO_MAGIC or tlv_total < 4:
        raise PackagerError("MCUboot 签名 TLV 不存在")
    end = tlv_offset + tlv_total
    if end > len(image):
        raise PackagerError("MCUboot TLV 长度错误")
    entries: dict[int, list[bytes]] = {}
    cursor = tlv_offset + 4
    while cursor < end:
        if cursor + 4 > end:
            raise PackagerError("MCUboot TLV 项头不完整")
        kind = image[cursor]
        length, = struct.unpack_from("<H", image, cursor + 2)
        cursor += 4
        if cursor + length > end:
            raise PackagerError("MCUboot TLV 项长度错误")
        entries.setdefault(kind, []).append(image[cursor:cursor + length])
        cursor += length
    calculated = hashlib.sha256(image[:tlv_offset]).digest()
    if entries.get(0x10) != [calculated]:
        raise PackagerError("MCUboot SHA-256 TLV 与镜像内容不一致")
    if len(entries.get(0x01, [])) != 1 or len(entries[0x01][0]) != 32:
        raise PackagerError("MCUboot KEYHASH TLV 缺失")
    if len(entries.get(0x22, [])) != 1:
        raise PackagerError("MCUboot ECDSA-P256 签名 TLV 缺失")
    return {
        "version": version,
        "application_bytes": image_size,
        "slot_bytes": len(image),
        "sha256": hashlib.sha256(image).hexdigest(),
        "signed_end": end,
    }


def _newest_source_mtime(project_dir: Path) -> float:
    mtimes = [
        path.stat().st_mtime
        for path in (project_dir / "src").rglob("*")
        if path.is_file() and path.suffix.lower() in {".c", ".h"}
    ]
    return max(mtimes, default=0.0)


def build_packages(
    *,
    project_dir: Path,
    elf_path: Path,
    config_path: Path,
    private_key: Path,
    template_signed_bin: Path,
    output_dir: Path,
    version: str,
    node_path: str = "",
    objcopy_path: str = "",
    log: Callable[[str], None] = lambda _message: None,
) -> PackageResult:
    version = normalize_version(version)
    source_version = read_source_version(config_path)
    if source_version != version:
        raise PackagerError(
            f"版本不一致：界面为 {version}，ota_update_config.h 为 {source_version}"
        )
    for label, path in (
        ("ELF", elf_path),
        ("ECDSA私钥", private_key),
        ("MCUboot模板", template_signed_bin),
    ):
        if not path.is_file():
            raise PackagerError(f"{label}文件不存在：{path}")
    if elf_path.stat().st_mtime < _newest_source_mtime(project_dir):
        raise PackagerError("ELF 比 src 中的源码旧，请先在 e² studio 执行 Clean + Build")

    node = find_node(node_path)
    objcopy = find_objcopy(objcopy_path)
    tools_dir = Path(__file__).resolve().parent
    sign_script = tools_dir / "sign_mcuboot_keyhash.js"
    ihex_script = tools_dir / "mcuboot_sparse_ihex.js"
    output_dir.mkdir(parents=True, exist_ok=True)

    unsigned_bin = output_dir / f"project_firdge_{version}_unsigned.bin"
    pending_bin = output_dir / f"project_firdge_{version}_signed_keyhash_pending.bin"
    confirmed_bin = output_dir / f"project_firdge_{version}_signed_keyhash_confirmed.bin"
    sparse_hex = output_dir / f"app_keyhash_lower_{version}_sparse.hex"

    log("[1/4] 从 ELF 提取应用二进制……")
    _run([objcopy, "-O", "binary", str(elf_path), str(unsigned_bin)], log)
    log("[2/4] 生成网站 OTA pending 镜像并执行 ECDSA 自检……")
    _run(
        [
            node, str(sign_script), str(unsigned_bin), str(private_key),
            str(template_signed_bin), version, str(pending_bin), "pending",
        ],
        log,
    )
    log("[3/4] 生成本地 USB confirmed 镜像……")
    _run(
        [
            node, str(sign_script), str(unsigned_bin), str(private_key),
            str(template_signed_bin), version, str(confirmed_bin), "confirmed",
        ],
        log,
    )
    log("[4/4] 生成本地 USB sparse HEX……")
    _run([node, str(ihex_script), str(confirmed_bin), hex(LOWER_SLOT_ADDRESS), str(sparse_hex)], log)

    pending_info = inspect_mcuboot_image(pending_bin, version)
    confirmed_info = inspect_mcuboot_image(confirmed_bin, version)
    if pending_info["application_bytes"] != confirmed_info["application_bytes"]:
        raise PackagerError("pending 与 confirmed 镜像应用长度不一致")
    log(f"本地校验通过：MCUboot {version} / ECDSA-P256 / {pending_info['slot_bytes']} bytes")
    return PackageResult(
        version=version,
        unsigned_bin=unsigned_bin,
        ota_pending_bin=pending_bin,
        local_confirmed_bin=confirmed_bin,
        local_sparse_hex=sparse_hex,
        application_bytes=int(pending_info["application_bytes"]),
        sha256=str(pending_info["sha256"]),
    )


class FirmwareServerClient:
    def __init__(self, base_url: str, timeout: float = 30.0):
        parsed = urllib.parse.urlsplit(base_url.strip())
        if parsed.scheme not in {"http", "https"} or not parsed.netloc:
            raise PackagerError("服务器地址必须以 http:// 或 https:// 开头")
        self.base_url = f"{parsed.scheme}://{parsed.netloc}"
        self.origin = self.base_url
        self.timeout = timeout
        self.csrf = ""
        cookie_jar = http.cookiejar.CookieJar()
        self.opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(cookie_jar))

    def _request(
        self,
        method: str,
        path: str,
        *,
        json_body: dict[str, object] | None = None,
        binary_body: bytes | None = None,
        content_type: str | None = None,
        timeout: float | None = None,
    ) -> dict[str, object]:
        headers = {"Accept": "application/json"}
        data = None
        if json_body is not None:
            data = json.dumps(json_body, ensure_ascii=False).encode("utf-8")
            headers["Content-Type"] = "application/json"
        elif binary_body is not None:
            data = binary_body
            headers["Content-Type"] = content_type or "application/octet-stream"
        if method.upper() not in {"GET", "HEAD", "OPTIONS"}:
            headers["Origin"] = self.origin
            if self.csrf and path != "/api/admin/login":
                headers["X-CSRF-Token"] = self.csrf
        request = urllib.request.Request(
            urllib.parse.urljoin(self.base_url, path),
            data=data,
            headers=headers,
            method=method.upper(),
        )
        try:
            with self.opener.open(request, timeout=timeout or self.timeout) as response:
                payload = response.read()
        except urllib.error.HTTPError as exc:
            payload = exc.read()
            try:
                detail = json.loads(payload.decode("utf-8"))
                message = str(detail.get("error") or detail.get("validation_error") or exc.reason)
                if detail.get("validation_error"):
                    message += f"：{detail['validation_error']}"
            except (ValueError, UnicodeDecodeError):
                message = str(exc.reason)
            raise PackagerError(f"服务器返回 HTTP {exc.code}：{message}") from exc
        except (OSError, urllib.error.URLError) as exc:
            raise PackagerError(f"无法连接服务器：{exc}") from exc
        try:
            result = json.loads(payload.decode("utf-8"))
        except (ValueError, UnicodeDecodeError) as exc:
            raise PackagerError("服务器没有返回有效 JSON") from exc
        if not isinstance(result, dict):
            raise PackagerError("服务器返回格式错误")
        return result

    def login(self, password: str) -> None:
        if not password:
            raise PackagerError("请输入网站管理员密码")
        result = self._request("POST", "/api/admin/login", json_body={"password": password})
        if not bool(result.get("authenticated")) or not result.get("csrf_token"):
            raise PackagerError("服务器登录失败")
        self.csrf = str(result["csrf_token"])

    def ensure_release(
        self,
        version: str,
        *,
        title: str,
        changelog: str,
    ) -> dict[str, object]:
        version = normalize_version(version)
        listed = self._request("GET", "/api/admin/firmware/releases")
        releases = listed.get("releases") or []
        for release in releases:
            if isinstance(release, dict) and release.get("component") == "main" and release.get("version") == version:
                if not bool(release.get("published")):
                    updated = self._request(
                        "PATCH",
                        f"/api/admin/firmware/releases/{int(release['id'])}",
                        json_body={"published": True},
                    )
                    return dict(updated.get("release") or release)
                return release
        created = self._request(
            "POST",
            "/api/admin/firmware/releases",
            json_body={
                "component": "main",
                "version": version,
                "title": title or f"RA6M5 {version}",
                "changelog": changelog,
                "channel": "stable",
                "published": True,
                "is_current": False,
            },
        )
        release = created.get("release")
        if not isinstance(release, dict):
            raise PackagerError("服务器创建固件版本后未返回版本记录")
        return release

    def upload_ota(self, release_id: int, image_path: Path) -> dict[str, object]:
        try:
            body = image_path.read_bytes()
        except OSError as exc:
            raise PackagerError(f"无法读取待上传镜像：{image_path}") from exc
        query = urllib.parse.urlencode(
            {
                "release_id": str(release_id),
                "kind": "ota",
                "filename": image_path.name,
                "target_part": TARGET_PART,
                "min_bootloader": MIN_BOOTLOADER,
            }
        )
        result = self._request(
            "POST",
            f"/api/admin/firmware/artifacts?{query}",
            binary_body=body,
            content_type="application/octet-stream",
            timeout=90.0,
        )
        artifact = result.get("artifact")
        if not isinstance(artifact, dict) or not bool(artifact.get("image_valid")):
            raise PackagerError("文件已上传，但服务器没有确认 image_valid=true")
        return artifact
