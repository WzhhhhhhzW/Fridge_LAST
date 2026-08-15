#!/usr/bin/env python3
"""RA6M5 + W800 + DeepSeek 服务器转发程序。

这个文件运行在你的 Ubuntu 24.04 服务器上，负责做三件事：

1. 监听一个 TCP 端口，等待 W800 从单片机侧连进来。
2. 收到单片机发来的一行文字后，调用 DeepSeek API。
3. 把 DeepSeek 的回复通过同一个 TCP 连接发回单片机。

单片机和服务器之间的协议非常简单：

- 单片机 -> 服务器：一行 UTF-8 文本，以换行符结束。
- 服务器 -> 单片机：普通文本回复，最后发送 [END]，让单片机知道本次回复结束。

重要安全原则：

- 不要把 API Key 写进这个文件。
- API Key 放到 /etc/fridge-deepseek.env 的 DEEPSEEK_API_KEY 里。
- systemd 启动服务时会自动读取那个环境变量文件。
"""

from __future__ import annotations

import asyncio
import base64
import contextlib
import hashlib
import hmac
import json
import logging
import mimetypes
import os
import re
import secrets
import signal
import time
import zlib
from collections import deque
from dataclasses import dataclass
from enum import Enum
from http.cookies import SimpleCookie
from pathlib import Path
from typing import Any, Deque, Dict, List
from urllib.parse import parse_qs, quote, unquote, urlparse

from openai import AsyncOpenAI

from dashboard import DashboardStore, medicine_expiry, normalize_production_date
from firmware_ota import (
    FirmwareImageError,
    failed_inspection,
    inspect_mcuboot_image,
    normalize_version,
    parse_version,
    version_is_newer,
)
from speech_pipeline import SpeechConfig, SpeechPipeline, SpeechPipelineError


# =========================
# 1. 可通过环境变量修改的配置
# =========================
#
# Linux 小白提示：
# - os.getenv("名字", "默认值") 的意思是：
#   如果系统环境变量里设置了这个名字，就用环境变量里的值；
#   如果没设置，就用后面的默认值。
# - 这些环境变量会写在 /etc/fridge-deepseek.env 里，由 systemd 服务读取。

# 监听地址。
# 0.0.0.0 表示监听服务器所有网卡，这样 W800 才能通过公网 IP 连进来。
HOST = os.getenv("FRIDGE_BIND_HOST", "0.0.0.0")

# 监听端口。
# 单片机程序里也写了 10086，所以服务器和单片机必须保持一致。
PORT = int(os.getenv("FRIDGE_BIND_PORT", "10086"))

# 手机 App 访问的 HTTP API 地址和端口。
# W800 继续使用 10086，手机 App 默认访问 10087。
HTTP_HOST = os.getenv("FRIDGE_HTTP_HOST", "0.0.0.0")
HTTP_PORT = int(os.getenv("FRIDGE_HTTP_PORT", "10087"))

# 手机 App 访问 API 时需要在请求头里带上：
# X-Fridge-Token: 这里填写的值
APP_TOKEN = os.getenv("FRIDGE_APP_TOKEN", "fridge-app-token")

# DeepSeek 模型名称。
MODEL = os.getenv("DEEPSEEK_MODEL", "deepseek-v4-pro")

# DeepSeek 的 OpenAI-compatible API 地址。
BASE_URL = os.getenv("DEEPSEEK_BASE_URL", "https://api.deepseek.com")

# 系统提示词。它决定 DeepSeek 回答时的角色和风格。
SYSTEM_PROMPT = os.getenv(
    "DEEPSEEK_SYSTEM_PROMPT",
    "你是冰箱项目的中文助手。回答要清楚、实用，适合串口终端阅读。",
)

# 每个 TCP 客户端保留多少轮上下文。
# 1 轮 = 用户问一次 + 助手答一次。设成 6 表示保留最近 6 轮对话。
MAX_HISTORY_PAIRS = int(os.getenv("DEEPSEEK_MAX_HISTORY_PAIRS", "6"))

# DeepSeek 请求最长等待时间，单位秒。
# 网络慢或模型回复很长时，可以在环境变量里适当调大。
REQUEST_TIMEOUT_SECONDS = float(os.getenv("DEEPSEEK_TIMEOUT_SECONDS", "90"))

# SSIE voice protocol and DashScope speech settings.
AUDIO_DEVICE_TOKEN = os.getenv("FRIDGE_AUDIO_TOKEN", "")
AUDIO_SAMPLE_RATE = 16000
AUDIO_FRAME_SAMPLES = 1600
AUDIO_MAX_FRAMES = 300
AUDIO_FRAME_TIMEOUT_SECONDS = 10.0
AUDIO_TTS_FRAME_BYTES = 3200
AUDIO_MAX_TTS_BYTES = 60 * AUDIO_SAMPLE_RATE * 2
AUDIO_SAY_MAX_TEXT_BYTES = 160
AUDIO_HEALTH_DIAGNOSIS_MAX_BYTES = 96
AUDIO_HEALTH_MEDICINE_MAX_BYTES = 160
speech_pipeline = SpeechPipeline(SpeechConfig.from_env())

VOICE_NO_MEDICINE_REPLY = "库存中没有对症药品"
HEALTH_NO_MEDICINE = "无对应药品"
VOICE_EMERGENCY_REPLY = "请立刻拨打120"
HEALTH_EMERGENCY_DIAGNOSIS = "紧急情况"
HEALTH_CARE_MAX_CHARS = 10


class HealthStatus(str, Enum):
    """Validated server-side outcome of one independent health assessment."""

    NORMAL = "normal"
    NO_MEDICINE = "no_medicine"
    EMERGENCY = "emergency"


@dataclass(frozen=True)
class HealthAssessment:
    status: HealthStatus
    diagnosis: str
    medicine: str
    care_phrase: str = ""

# 药瓶仓储 JSON 保存文件。
# 默认保存在 /opt/fridge-deepseek 目录下，服务器重启后数据还在。
MEDICINE_DB_PATH = Path(os.getenv("MEDICINE_DB_PATH", "/opt/fridge-deepseek/medicine_store.json"))
DASHBOARD_DB_PATH = Path(os.getenv("DASHBOARD_DB_PATH", "/opt/fridge-deepseek/dashboard.db"))
DASHBOARD_WEB_ROOT = Path(os.getenv("DASHBOARD_WEB_ROOT", str(Path(__file__).with_name("web"))))
FIRMWARE_ROOT = Path(os.getenv("FIRMWARE_ROOT", "/opt/fridge-deepseek/firmware"))
OTA_DEVICE_TOKEN = os.getenv("FRIDGE_OTA_TOKEN", "")
OTA_DEVICE_ID = os.getenv("FRIDGE_OTA_DEVICE_ID", "fridge-main-01").strip()
OTA_TARGET_PART = os.getenv("FRIDGE_OTA_TARGET_PART", "R7FA6M5BF2CBG").strip()
OTA_DEVICE_WRITERS: Dict[str, asyncio.StreamWriter] = {}
OTA_PUBLIC_KEY_TEXT = os.getenv("FRIDGE_FIRMWARE_PUBLIC_KEY", "").strip()
OTA_PUBLIC_KEY_PATH = Path(OTA_PUBLIC_KEY_TEXT) if OTA_PUBLIC_KEY_TEXT else None
OTA_CHUNK_BYTES = 4096
OTA_MAX_IMAGE_BYTES = 512 * 1024
ADMIN_PASSWORD_HASH = os.getenv("DASHBOARD_ADMIN_PASSWORD_HASH", "").strip()
ALLOW_INSECURE_ADMIN = os.getenv("DASHBOARD_ALLOW_INSECURE_ADMIN", "0").strip().lower() in {
    "1", "true", "yes", "on",
}
ADMIN_COOKIE_SECURE = os.getenv("DASHBOARD_COOKIE_SECURE", "0").strip().lower() in {
    "1", "true", "yes", "on",
}
ADMIN_SESSION_TTL_SECONDS = 8 * 60 * 60
ADMIN_SESSION_COOKIE = "fridge_admin_session"
ADMIN_PBKDF2_ROUNDS = 600_000
ADMIN_LOGIN_WINDOW_SECONDS = 10 * 60
ADMIN_LOGIN_LOCK_SECONDS = 15 * 60
ADMIN_LOGIN_MAX_FAILURES = 5
MAX_JSON_BODY_BYTES = 256 * 1024
MAX_FIRMWARE_UPLOAD_BYTES = 256 * 1024 * 1024
FIRMWARE_COMPONENTS = {"main", "hmi", "vision"}
FIRMWARE_KINDS = {"install", "source", "model", "project", "ota"}
FIRMWARE_CHANNELS = {"stable", "beta", "legacy"}
FIRMWARE_ALLOWED_SUFFIXES = {
    ".srec", ".elf", ".bin", ".hex", ".hmi", ".tft", ".zip", ".eim",
    ".tflite", ".onnx", ".kmodel", ".tar", ".gz",
}
MEDICINE_SLOT_COUNT = 32
MEDICINE_QUANTITY_MAX = 999
MEDICINE_UNKNOWN = "未知"
MEDICINE_FIELD_LIMITS = {
    "name": 48,
    "production_date": 16,
    "shelf_life": 24,
    "efficacy": 160,
    "symptom": 160,
}
DEFAULT_MEDICINE_NAMES = [
    "999感冒灵",
    "复方黄连素片",
    "头孢克肟胶囊",
    "强力枇杷露",
    "晚A茶",
    "未识别到物品灵",
    "氨麻苯美片",
    "氯雷它定片",
    "莲花淸瘟胶嚢",
    "未录入药品",
]

medicine_lock = asyncio.Lock()
fridge_lock = asyncio.Lock()

FRIDGE_ONLINE_TIMEOUT_SECONDS = 8.0
VISION_SLOT_NONE = 255
VISION_UNKNOWN_CODE = 0x0E
VISION_CLASS_NAMES: Dict[int, str] = {
    0x00: "999感冒灵",
    0x01: "云南白药气雾剂",
    0x02: "地奎氨铵含片",
    0x03: "头孢克肟胶囊",
    0x04: "头孢地尼胶囊",
    0x05: "对乙酰氨基酚缓释片",
    0x06: "晚A茶",
    0x07: "枸橼酸莫沙必利片",
    0x08: "氨麻苯美片",
    0x09: "磷酸奥司他韦胶囊",
    0x0A: "肠炎宁片",
    0x0B: "莲花清瘟胶囊",
    0x0C: "蒙脱石散",
    0x0D: "诺氟沙星胶囊",
    VISION_UNKNOWN_CODE: "未识别到库存中药品",
}

fridge_state: Dict[str, Any] = {
    "online": False,
    "last_seen": 0.0,
    "set_temp": 1,
    "current_temp": 0.0,
    "fan": 0,
    "cool": False,
    "door": False,
    "water_cool": False,
    "vision_slot": None,
    "vision_code": None,
    "vision_name": "",
    "vision_updated_at": 0.0,
    "power_40_w": None,
    "power_41_w": None,
    "power_total_w": None,
    "network_rtt_ms": None,
    "heartbeat_gap_ms": None,
}

fridge_control: Dict[str, Any] = {
    "set_temp": 1,
    "fan": 0,
    "cool": False,
    # door 是一次性开门命令，发给单片机后会自动清零，避免舵机反复开门。
    "door": False,
    "water_cool": False,
}
fridge_control_initialized = False
fridge_control_dirty = False
dashboard_store: DashboardStore | None = None
pending_control_command: Dict[str, Any] | None = None
pending_door_command: Dict[str, Any] | None = None
last_control_command: Dict[str, Any] | None = None
admin_login_failures: Dict[str, Deque[float]] = {}
admin_login_locked_until: Dict[str, float] = {}


# =========================
# 2. 日志配置
# =========================
#
# 日志可以通过下面命令查看：
# sudo journalctl -u fridge-deepseek.service -f
logging.basicConfig(
    level=os.getenv("LOG_LEVEL", "INFO"),
    format="%(asctime)s %(levelname)s %(message)s",
)
logger = logging.getLogger("fridge-deepseek")


class DashboardLogHandler(logging.Handler):
    """Mirror sanitized application logs into SQLite for the admin log page."""

    def __init__(self) -> None:
        super().__init__(level=logging.INFO)
        self.busy = False

    def emit(self, record: logging.LogRecord) -> None:
        if self.busy or dashboard_store is None:
            return
        self.busy = True
        try:
            message = record.getMessage()
            message = re.sub(
                r"(?i)(token|api[_ -]?key|password)\s*[=:]\s*[^\s,;]+",
                r"\1=[REDACTED]",
                message,
            )
            dashboard_store.add_log(record.levelname, record.name, message, timestamp=record.created)
        except Exception:
            pass
        finally:
            self.busy = False


def dashboard_metric(channel: str, operation: str, success: bool, started_at: float) -> None:
    """Record a request metric without affecting the device protocol if metrics fail."""
    if dashboard_store is None:
        return
    try:
        dashboard_store.record_metric(
            channel,
            operation,
            success,
            (time.perf_counter() - started_at) * 1000.0,
        )
    except Exception:
        logger.exception("dashboard metric write failed")


def dashboard_event(category: str, severity: str, title: str, detail: str = "") -> None:
    """Write a sanitized dashboard event without breaking the primary service."""
    if dashboard_store is None:
        return
    try:
        dashboard_store.add_event(category, severity, title, detail)
    except Exception:
        logger.exception("dashboard event write failed")


def dashboard_health(symptom: str,
                     diagnosis: str,
                     medicine: str,
                     success: bool,
                     latency_ms: float) -> None:
    if dashboard_store is None:
        return
    try:
        dashboard_store.add_health(symptom, diagnosis, medicine, success, latency_ms)
    except Exception:
        logger.exception("dashboard health write failed")


def dashboard_audit(actor: str,
                    category: str,
                    action: str,
                    *,
                    target: str = "",
                    summary: str = "",
                    result: str = "accepted",
                    command_id: str | None = None) -> None:
    """Write a sanitized operation record without affecting the main protocol."""
    if dashboard_store is None:
        return
    try:
        dashboard_store.add_audit(
            actor,
            category,
            action,
            target=target,
            summary=summary,
            result=result,
            command_id=command_id,
        )
    except Exception:
        logger.exception("dashboard audit write failed")


def _urlsafe_b64(value: bytes) -> str:
    return base64.urlsafe_b64encode(value).decode("ascii").rstrip("=")


def _urlsafe_b64decode(value: str) -> bytes:
    return base64.urlsafe_b64decode(value + "=" * (-len(value) % 4))


def make_admin_password_hash(password: str, *, salt: bytes | None = None) -> str:
    """Return the value used by DASHBOARD_ADMIN_PASSWORD_HASH."""
    if len(password) < 10:
        raise ValueError("管理员密码至少需要10个字符")
    salt = salt or secrets.token_bytes(16)
    digest = hashlib.pbkdf2_hmac(
        "sha256",
        password.encode("utf-8"),
        salt,
        ADMIN_PBKDF2_ROUNDS,
    )
    return (
        f"pbkdf2_sha256${ADMIN_PBKDF2_ROUNDS}$"
        f"{_urlsafe_b64(salt)}${_urlsafe_b64(digest)}"
    )


def verify_admin_password(password: str, encoded: str | None = None) -> bool:
    encoded = (encoded if encoded is not None else ADMIN_PASSWORD_HASH).strip()
    try:
        algorithm, rounds_text, salt_text, digest_text = encoded.split("$", 3)
        if algorithm != "pbkdf2_sha256":
            return False
        rounds = int(rounds_text)
        if rounds < 100_000 or rounds > 2_000_000:
            return False
        salt = _urlsafe_b64decode(salt_text)
        expected = _urlsafe_b64decode(digest_text)
        actual = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt, rounds)
        return hmac.compare_digest(actual, expected)
    except (ValueError, TypeError, base64.binascii.Error):
        return False


def session_token_hash(token: str) -> str:
    return hashlib.sha256(token.encode("utf-8")).hexdigest()


def make_app_client_token() -> str:
    return "frg_" + secrets.token_urlsafe(32)


def app_token_prefix(token: str) -> str:
    return token[:10] + "…" if len(token) > 10 else token


def cookie_value(headers: Dict[str, str], name: str) -> str:
    raw = headers.get("cookie", "")
    if not raw:
        return ""
    try:
        cookies = SimpleCookie()
        cookies.load(raw)
        morsel = cookies.get(name)
        return morsel.value if morsel is not None else ""
    except Exception:
        return ""


def admin_session_from_headers(headers: Dict[str, str], *, touch: bool = True) -> Dict[str, Any] | None:
    if dashboard_store is None:
        return None
    token = cookie_value(headers, ADMIN_SESSION_COOKIE)
    if not token:
        return None
    return dashboard_store.admin_session(session_token_hash(token), touch=touch)


def origin_matches_host(headers: Dict[str, str]) -> bool:
    origin = headers.get("origin", "")
    host = headers.get("host", "")
    if not origin or not host:
        return False
    parsed = urlparse(origin)
    return parsed.scheme in {"http", "https"} and parsed.netloc.lower() == host.lower()


def admin_cookie_header(token: str, *, clear: bool = False) -> str:
    max_age = 0 if clear else ADMIN_SESSION_TTL_SECONDS
    parts = [
        f"{ADMIN_SESSION_COOKIE}={token}",
        "Path=/",
        "HttpOnly",
        "SameSite=Strict",
        f"Max-Age={max_age}",
    ]
    if ADMIN_COOKIE_SECURE:
        parts.append("Secure")
    return "; ".join(parts)


def authenticate_http_request(method: str,
                              headers: Dict[str, str]) -> tuple[Dict[str, Any] | None, str | None]:
    """Accept a per-phone App token, the legacy token, or an admin session."""
    provided_token = headers.get("x-fridge-token", "")
    if provided_token:
        token_hash = session_token_hash(provided_token)
        client_record: Dict[str, Any] | None = None
        if dashboard_store is not None:
            client_record = dashboard_store.client_by_token_hash(token_hash)
            if client_record is None and APP_TOKEN and hmac.compare_digest(provided_token, APP_TOKEN):
                client_record = dashboard_store.ensure_legacy_client(
                    token_hash,
                    app_token_prefix(provided_token),
                )
            if client_record is not None:
                if not bool(client_record.get("enabled")):
                    return None, "client_disabled"
                dashboard_store.touch_client(int(client_record["id"]))
                return {
                    "type": "app",
                    "actor": f"mobile_client:{client_record['id']}",
                    "client": client_record,
                }, None
        elif APP_TOKEN and hmac.compare_digest(provided_token, APP_TOKEN):
            return {"type": "app", "actor": "mobile_app"}, None

    session = admin_session_from_headers(headers)
    if session is None:
        return None, "unauthorized"
    if method not in {"GET", "HEAD", "OPTIONS"}:
        csrf = headers.get("x-csrf-token", "")
        if not csrf or not hmac.compare_digest(csrf, str(session["csrf_token"])):
            return None, "csrf"
        if not origin_matches_host(headers):
            return None, "origin"
    return {"type": "admin", "actor": "web_admin", "session": session}, None


def login_rate_state(peer_ip: str, *, now: float | None = None) -> tuple[bool, int]:
    now = now or time.time()
    locked_until = admin_login_locked_until.get(peer_ip, 0.0)
    if locked_until > now:
        return False, max(1, int(locked_until - now))
    failures = admin_login_failures.setdefault(peer_ip, deque())
    while failures and failures[0] <= now - ADMIN_LOGIN_WINDOW_SECONDS:
        failures.popleft()
    if len(failures) >= ADMIN_LOGIN_MAX_FAILURES:
        admin_login_locked_until[peer_ip] = now + ADMIN_LOGIN_LOCK_SECONDS
        failures.clear()
        return False, ADMIN_LOGIN_LOCK_SECONDS
    return True, 0


def record_login_failure(peer_ip: str, *, now: float | None = None) -> None:
    now = now or time.time()
    failures = admin_login_failures.setdefault(peer_ip, deque())
    failures.append(now)
    if len(failures) >= ADMIN_LOGIN_MAX_FAILURES:
        admin_login_locked_until[peer_ip] = now + ADMIN_LOGIN_LOCK_SECONDS


def clear_login_failures(peer_ip: str) -> None:
    admin_login_failures.pop(peer_ip, None)
    admin_login_locked_until.pop(peer_ip, None)


def make_blank_medicine_record(slot: int) -> Dict[str, Any]:
    """创建一个空药瓶槽位。"""
    return {
        "valid": False,
        "slot": slot,
        "name": "",
        "production_date": "",
        "shelf_life": "",
        "efficacy": "",
        "quantity": 0,
    }


def make_default_medicine_records() -> List[Dict[str, Any]]:
    """首次启动时创建 32 个药瓶槽位，并导入旧代码里的 0-9 药品名。"""
    records: List[Dict[str, Any]] = []
    for slot in range(MEDICINE_SLOT_COUNT):
        if slot < len(DEFAULT_MEDICINE_NAMES):
            records.append(
                {
                    "valid": True,
                    "slot": slot,
                    "name": DEFAULT_MEDICINE_NAMES[slot],
                    "production_date": MEDICINE_UNKNOWN,
                    "shelf_life": MEDICINE_UNKNOWN,
                    "efficacy": MEDICINE_UNKNOWN,
                    "quantity": 1,
                }
            )
        else:
            records.append(make_blank_medicine_record(slot))
    return records


def normalize_medicine_record(raw: Any, slot: int) -> Dict[str, Any]:
    """把 JSON 文件里的记录整理成程序固定需要的字段。"""
    if not isinstance(raw, dict):
        return make_blank_medicine_record(slot)

    valid = bool(raw.get("valid", False))
    record = make_blank_medicine_record(slot)
    quantity_default = 1 if valid else 0
    try:
        quantity = int(raw.get("quantity", quantity_default))
    except (TypeError, ValueError):
        quantity = quantity_default
    quantity = max(0, min(MEDICINE_QUANTITY_MAX, quantity))
    if not valid or quantity == 0:
        return make_blank_medicine_record(slot)

    record["valid"] = True
    record["quantity"] = quantity
    for key in ("name", "production_date", "shelf_life", "efficacy"):
        value = raw.get(key, "")
        record[key] = str(value) if value is not None else ""
    if record["production_date"].strip() not in {"", MEDICINE_UNKNOWN}:
        try:
            record["production_date"] = normalize_production_date(record["production_date"])
        except ValueError:
            # Keep malformed legacy data visible so the dashboard can mark it unknown.
            pass
    return record


def save_medicine_records(records: List[Dict[str, Any]]) -> None:
    """保存药瓶数据库，使用临时文件替换，降低写坏 JSON 的概率。"""
    MEDICINE_DB_PATH.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = MEDICINE_DB_PATH.with_suffix(MEDICINE_DB_PATH.suffix + ".tmp")
    payload = {"records": records}
    tmp_path.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
    os.replace(tmp_path, MEDICINE_DB_PATH)


def load_medicine_records() -> List[Dict[str, Any]]:
    """读取药瓶数据库；不存在时自动创建默认数据库。"""
    if not MEDICINE_DB_PATH.exists():
        records = make_default_medicine_records()
        save_medicine_records(records)
        return records

    try:
        payload = json.loads(MEDICINE_DB_PATH.read_text(encoding="utf-8"))
    except Exception:
        logger.exception("medicine db is broken, rebuild default db: %s", MEDICINE_DB_PATH)
        records = make_default_medicine_records()
        save_medicine_records(records)
        return records

    raw_records = payload.get("records", payload) if isinstance(payload, dict) else payload
    if not isinstance(raw_records, list):
        records = make_default_medicine_records()
        save_medicine_records(records)
        return records

    records = [
        normalize_medicine_record(raw_records[slot] if slot < len(raw_records) else None, slot)
        for slot in range(MEDICINE_SLOT_COUNT)
    ]
    if raw_records != records:
        save_medicine_records(records)
    return records


def parse_medicine_operation(user_text: str) -> tuple[str, str]:
    """解析 MED 命令，兼容 MED ADD xxx 和 MED ADD|xxx 两种写法。"""
    rest = user_text[3:].strip()
    if not rest:
        return "", ""

    if "|" in rest and " " not in rest.split("|", 1)[0].strip():
        op, payload = rest.split("|", 1)
    else:
        op, _, payload = rest.partition(" ")

    return op.strip().upper(), payload.strip()


def parse_slot(slot_text: str) -> int:
    """解析并检查槽位号。"""
    try:
        slot = int(slot_text.strip(), 10)
    except ValueError as exc:
        raise ValueError("slot must be a number") from exc

    if slot < 0 or slot >= MEDICINE_SLOT_COUNT:
        raise ValueError(f"slot must be 0..{MEDICINE_SLOT_COUNT - 1}")
    return slot


def parse_quantity(quantity_text: str, *, allow_zero: bool = False) -> int:
    """Parse a medicine quantity with a fixed 0..999 storage range."""
    try:
        quantity = int(str(quantity_text).strip(), 10)
    except (TypeError, ValueError) as exc:
        raise ValueError("quantity must be a number") from exc
    minimum = 0 if allow_zero else 1
    if quantity < minimum or quantity > MEDICINE_QUANTITY_MAX:
        raise ValueError(f"quantity must be {minimum}..{MEDICINE_QUANTITY_MAX}")
    return quantity


def validate_medicine_field(key: str, value: str, *, allow_empty: bool = False) -> str:
    """检查药品字段，避免协议分隔符和换行破坏单片机解析。"""
    value = value.strip()
    if not allow_empty and not value:
        raise ValueError(f"{key} is empty")
    if "|" in value or "\n" in value or "\r" in value:
        raise ValueError(f"{key} contains invalid character")
    if len(value) >= MEDICINE_FIELD_LIMITS[key]:
        raise ValueError(f"{key} is too long")
    if key == "production_date" and value != MEDICINE_UNKNOWN:
        value = normalize_production_date(value)
    return value


def split_medicine_fields(payload: str, field_count: int) -> List[str]:
    """按竖线分隔字段，并检查数量。"""
    fields = [field.strip() for field in payload.split("|")]
    if len(fields) != field_count:
        raise ValueError(f"need {field_count} fields")
    return fields


def split_medicine_fields_range(payload: str, minimum: int, maximum: int) -> List[str]:
    fields = [field.strip() for field in payload.split("|")]
    if len(fields) < minimum or len(fields) > maximum:
        raise ValueError(f"need {minimum} or {maximum} fields")
    return fields


def format_medicine_record(record: Dict[str, Any]) -> str:
    """把一条记录格式化成单片机容易解析的一行。"""
    return (
        f"{int(record['slot'])}|{1 if record.get('valid') else 0}|"
        f"{record.get('name', '')}|{record.get('production_date', '')}|"
        f"{record.get('shelf_life', '')}|{record.get('efficacy', '')}"
    )


def format_medicine_record_with_quantity(record: Dict[str, Any]) -> str:
    """Versioned record shape used only by MED GETQ."""
    return (
        f"{int(record['slot'])}|{1 if record.get('valid') else 0}|"
        f"{int(record.get('quantity', 0))}|{record.get('name', '')}|"
        f"{record.get('production_date', '')}|{record.get('shelf_life', '')}|"
        f"{record.get('efficacy', '')}"
    )


def sanitize_medicine_protocol_field(value: Any) -> str:
    """Keep LISTFULL field and record delimiters unambiguous for the MCU."""
    text = str(value if value is not None else "")
    return " ".join(text.replace("|", " ").replace("\r", " ").replace("\n", " ").split())


def format_medicine_inventory_record(record: Dict[str, Any]) -> str:
    """Return name|quantity|production date|shelf life for MED LISTFULL."""
    return "|".join(
        (
            sanitize_medicine_protocol_field(record.get("name", "")) or MEDICINE_UNKNOWN,
            str(int(record.get("quantity", 0))),
            sanitize_medicine_protocol_field(record.get("production_date", "")) or MEDICINE_UNKNOWN,
            sanitize_medicine_protocol_field(record.get("shelf_life", "")) or MEDICINE_UNKNOWN,
        )
    )


def apply_medicine_quantity(records: List[Dict[str, Any]],
                            slot: int,
                            operation: str,
                            value: int) -> int:
    """Atomically mutate one record while the caller holds medicine_lock."""
    record = records[slot]
    operation = operation.upper()
    if not record.get("valid"):
        raise ValueError("EMPTY")

    current = int(record.get("quantity", 0))
    if operation == "QTY" or operation == "SET":
        new_quantity = value
    elif operation == "RESTOCK":
        new_quantity = current + value
        if new_quantity > MEDICINE_QUANTITY_MAX:
            raise ValueError("RANGE")
    elif operation == "TAKE":
        if value > current:
            raise ValueError("INSUFFICIENT")
        new_quantity = current - value
    else:
        raise ValueError("unknown quantity operation")

    if new_quantity == 0:
        records[slot] = make_blank_medicine_record(slot)
    else:
        record["quantity"] = new_quantity
    return new_quantity


def medicine_ok(detail: str) -> str:
    return f"[MED] OK {detail}"


def medicine_err(reason: str) -> str:
    return f"[MED] ERR {reason}"


def health_ok(diagnosis: str, medicine: str) -> str:
    return f"[HA] OK {diagnosis}|{medicine}"


def health_err(reason: str) -> str:
    return f"[HA] ERR {reason}"


def validate_symptom_field(value: str) -> str:
    """检查健康助手症状文本。"""
    value = value.strip()
    if not value:
        raise ValueError("symptom is empty")
    if "|" in value or "\n" in value or "\r" in value:
        raise ValueError("symptom contains invalid character")
    if len(value) >= MEDICINE_FIELD_LIMITS["symptom"]:
        raise ValueError("symptom is too long")
    return value


def clamp_int(value: int, low: int, high: int) -> int:
    return max(low, min(high, value))


def parse_bool_value(value: Any) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return bool(value)
    if isinstance(value, str):
        return value.strip().lower() in {"1", "true", "on", "yes", "open"}
    return False


def parse_optional_metric(value: str, *, scale: float = 1.0) -> float | None:
    text = str(value).strip().upper()
    if text in {"", "NA", "N/A", "NONE"}:
        return None
    return float(text) * scale


def fridge_snapshot_unlocked() -> Dict[str, Any]:
    now = time.time()
    online = (now - float(fridge_state.get("last_seen", 0.0))) <= FRIDGE_ONLINE_TIMEOUT_SECONDS
    fridge_state["online"] = online
    snapshot = dict(fridge_state)
    # The dashboard and Android status API must not keep presenting a stale
    # sensor value after the MCU has gone offline. Keep the last real reading
    # in fridge_state for reconnect/debugging, but expose 0.0 while offline.
    if not online:
        snapshot["current_temp"] = 0.0
        snapshot["power_40_w"] = None
        snapshot["power_41_w"] = None
        snapshot["power_total_w"] = None
        snapshot["network_rtt_ms"] = None
        snapshot["heartbeat_gap_ms"] = None
    snapshot["last_seen_age_seconds"] = None if not fridge_state["last_seen"] else round(now - fridge_state["last_seen"], 1)
    return snapshot


def public_command(command: Dict[str, Any] | None) -> Dict[str, Any] | None:
    if command is None:
        return None
    return {
        "id": command.get("id"),
        "requested_at": command.get("requested_at"),
        "status": command.get("status"),
        "fields": dict(command.get("fields") or {}),
        "door_status": command.get("door_status"),
        "confirmed_at": command.get("confirmed_at"),
    }


def set_command_result(command: Dict[str, Any] | None,
                       result: str,
                       *,
                       confirmed_at: float | None = None) -> None:
    if command is None:
        return
    if command.get("status") == result and (
        confirmed_at is None or command.get("confirmed_at") == confirmed_at
    ):
        return
    command["status"] = result
    if confirmed_at is not None:
        command["confirmed_at"] = confirmed_at
    if dashboard_store is not None:
        try:
            dashboard_store.update_audit_command(
                str(command.get("id") or ""),
                result,
                confirmed_at=confirmed_at,
            )
        except Exception:
            logger.exception("command audit update failed")


def refresh_control_commands_unlocked(*, now: float | None = None) -> None:
    """Mark commands late without cancelling the target that will apply after reconnect."""
    now = now or time.time()
    for command in (pending_control_command, pending_door_command):
        if command is None or command.get("status") not in {"pending", "queued"}:
            continue
        if now - float(command.get("requested_at") or now) > FRIDGE_ONLINE_TIMEOUT_SECONDS:
            set_command_result(command, "timeout")


def new_control_command(fields: Dict[str, Any], actor: str) -> Dict[str, Any]:
    return {
        "id": secrets.token_hex(8),
        "requested_at": time.time(),
        "status": "pending" if any(key != "door" for key in fields) else "queued",
        "fields": dict(fields),
        "door_status": "queued" if fields.get("door") else None,
        "confirmed_at": None,
        "actor": actor,
    }


def medicine_record_for_api(record: Dict[str, Any]) -> Dict[str, Any]:
    return {
        "valid": bool(record.get("valid")),
        "slot": int(record.get("slot", 0)),
        "quantity": int(record.get("quantity", 0)),
        "name": str(record.get("name", "")),
        "production_date": str(record.get("production_date", "")),
        "shelf_life": str(record.get("shelf_life", "")),
        "efficacy": str(record.get("efficacy", "")),
    }


def vision_name_for_code(code: int | None) -> str:
    if code is None:
        return ""
    if code not in VISION_CLASS_NAMES:
        raise ValueError("vision code must be 0..14 or 255")
    return VISION_CLASS_NAMES[code]


async def handle_fridge_status_command(user_text: str) -> str:
    """处理 MCU 每秒上报的冰箱状态，并返回手机端设置的目标值。"""
    global fridge_control_initialized, fridge_control_dirty
    global pending_control_command, pending_door_command

    payload = user_text[len("FRIDGE STATUS "):].strip()
    fields = [field.strip() for field in payload.split("|")]
    # 6/7 fields are deployed firmware; 10 fields add water cooling, two
    # INA226 channels and the previous status request RTT.
    if len(fields) not in (6, 7, 10):
        return "[FRIDGE] ERR need 6, 7 or 10 fields"

    try:
        set_temp_value = clamp_int(int(fields[0], 10), -40, 80)
        temp_x10 = int(fields[1], 10)
        fan_value = clamp_int(int(fields[2], 10), 0, 2)
        cool_value = parse_bool_value(fields[3])
        door_value = parse_bool_value(fields[4])
        vision_code_raw = int(fields[5], 10)
        if vision_code_raw == VISION_SLOT_NONE:
            vision_code = None
        elif vision_code_raw in VISION_CLASS_NAMES:
            vision_code = vision_code_raw
        else:
            raise ValueError("vision code must be 0..14 or 255")
        water_cool_value = (
            parse_bool_value(fields[6])
            if len(fields) >= 7
            else bool(fridge_state.get("water_cool", False))
        )
        power_40_raw = parse_optional_metric(fields[7], scale=0.1) if len(fields) == 10 else None
        power_41_raw = parse_optional_metric(fields[8], scale=0.1) if len(fields) == 10 else None
        power_40_w = round(abs(power_40_raw), 1) if power_40_raw is not None else None
        power_41_w = round(abs(power_41_raw), 1) if power_41_raw is not None else None
        network_rtt_ms = parse_optional_metric(fields[9]) if len(fields) == 10 else None
    except (TypeError, ValueError) as exc:
        return f"[FRIDGE] ERR {exc}"

    power_total_w = None
    if power_40_w is not None or power_41_w is not None:
        power_total_w = round((power_40_w or 0.0) + (power_41_w or 0.0), 1)

    new_power_alert: Dict[str, Any] | None = None
    new_vision_recognition: tuple[int, str, float] | None = None
    async with fridge_lock:
        now = time.time()
        previous_seen = float(fridge_state.get("last_seen") or 0.0)
        heartbeat_gap_ms = round((now - previous_seen) * 1000.0, 1) if previous_seen else None
        refresh_control_commands_unlocked(now=now)
        fridge_state.update(
            {
                "online": True,
                "last_seen": now,
                "set_temp": set_temp_value,
                "current_temp": round(temp_x10 / 10.0, 1),
                "fan": fan_value,
                "cool": cool_value,
                "door": door_value,
                "water_cool": water_cool_value,
                "power_40_w": power_40_w,
                "power_41_w": power_41_w,
                "power_total_w": power_total_w,
                "network_rtt_ms": network_rtt_ms,
                "heartbeat_gap_ms": heartbeat_gap_ms,
            }
        )
        if dashboard_store is not None:
            new_power_alert = dashboard_store.evaluate_power(power_total_w, cool_value, timestamp=now)

        if vision_code is not None:
            vision_name = vision_name_for_code(vision_code)
            # FRIDGE STATUS is sent once per second and may keep carrying the
            # previous class. Only a real class/name change is a new recognition
            # event, otherwise the dashboard event table would be flooded.
            if (
                fridge_state.get("vision_code") != vision_code
                or fridge_state.get("vision_name") != vision_name
            ):
                # Keep vision_slot as a compatibility alias for deployed APIs.
                fridge_state["vision_slot"] = vision_code
                fridge_state["vision_code"] = vision_code
                fridge_state["vision_name"] = vision_name
                fridge_state["vision_updated_at"] = now
                new_vision_recognition = (vision_code, vision_name, now)

        if not fridge_control_initialized:
            if not fridge_control_dirty:
                fridge_control["set_temp"] = set_temp_value
                fridge_control["fan"] = fan_value
                fridge_control["cool"] = cool_value
                fridge_control["door"] = False
                fridge_control["water_cool"] = water_cool_value
            fridge_control_initialized = True

        target_set_temp = clamp_int(int(fridge_control["set_temp"]), -40, 80)
        target_fan = clamp_int(int(fridge_control["fan"]), 0, 2)
        target_cool = 1 if bool(fridge_control["cool"]) else 0
        target_water_cool = 1 if bool(fridge_control["water_cool"]) else 0

        if fridge_control_dirty:
            if (
                set_temp_value == target_set_temp
                and fan_value == target_fan
                and cool_value == bool(fridge_control["cool"])
                and water_cool_value == bool(fridge_control["water_cool"])
            ):
                fridge_control_dirty = False
                if pending_control_command is not None:
                    set_command_result(pending_control_command, "confirmed", confirmed_at=now)
                    pending_control_command = None
        else:
            fridge_control["set_temp"] = set_temp_value
            fridge_control["fan"] = fan_value
            fridge_control["cool"] = cool_value
            fridge_control["water_cool"] = water_cool_value
            target_set_temp = set_temp_value
            target_fan = fan_value
            target_cool = 1 if cool_value else 0
            target_water_cool = 1 if water_cool_value else 0

        target_door = 1 if bool(fridge_control["door"]) else 0
        fridge_control["door"] = False
        if target_door and pending_door_command is not None:
            door_command = pending_door_command
            door_command["door_status"] = "sent"
            if door_command.get("status") != "confirmed" and door_command is not pending_control_command:
                set_command_result(door_command, "sent", confirmed_at=now)
            pending_door_command = None

    if new_power_alert is not None and new_power_alert.get("newly_created"):
        dashboard_event("power", "danger", "制冷功率异常", str(new_power_alert.get("detail", "")))
        logger.warning("power anomaly: %s", new_power_alert.get("detail", ""))

    if new_vision_recognition is not None and dashboard_store is not None:
        vision_code, vision_name, vision_seen_at = new_vision_recognition
        if dashboard_store.record_vision_recognition(
            vision_code,
            vision_name,
            include_in_recent=(vision_code != VISION_UNKNOWN_CODE),
            timestamp=vision_seen_at,
        ):
            dashboard_event(
                "vision",
                "warning" if vision_code == VISION_UNKNOWN_CODE else "info",
                f"视觉识别：{vision_name}",
                f"类别编号 0x{vision_code:02X}",
            )

    return (
        f"[FRIDGE] OK {target_set_temp}|{target_fan}|{target_cool}|"
        f"{target_door}|{target_water_cool}"
    )


async def ask_medicine_recommendation(client: AsyncOpenAI,
                                      symptom: str,
                                      records: List[Dict[str, Any]]) -> str:
    """让 DeepSeek 只在药箱已有药品里选一个药名。"""
    valid_records = [
        record for record in records
        if record.get("valid") and int(record.get("quantity", 0)) > 0 and record.get("name")
    ]
    if not valid_records:
        return "无对应药品"

    medicine_lines = [
        f"{record.get('name', '')}: {record.get('efficacy', '') or MEDICINE_UNKNOWN}"
        for record in valid_records
    ]
    medicine_names = [str(record.get("name", "")) for record in valid_records]

    response = await client.chat.completions.create(
        model=MODEL,
        messages=[
            {
                "role": "system",
                "content": (
                    "你是药箱药品匹配助手。只根据用户药箱里已有药品的药名和药效，"
                    "为症状选择最合适的一个药品名。不要解释，不要给剂量，不要推荐药箱外药品。"
                    "如果有明显相关药效，要积极返回对应药品名；只有药箱为空、明显无关或完全无法判断时，"
                    "才返回“无对应药品”。输出必须只有一个药品名，或“无对应药品”。"
                ),
            },
            {
                "role": "user",
                "content": (
                    f"症状：{symptom}\n"
                    "药箱药品：\n"
                    + "\n".join(medicine_lines)
                    + "\n请只返回一个药品名，或无对应药品。"
                ),
            },
        ],
        stream=False,
    )

    content = (response.choices[0].message.content or "").strip()
    first_line = content.splitlines()[0].strip() if content else ""
    first_line = first_line.strip(" \t\r\n。.:：\"'“”")
    if "无对应药品" in first_line:
        return "无对应药品"

    for name in medicine_names:
        if name and name in first_line:
            return name

    for name in medicine_names:
        if name and name in content:
            return name

    return "无对应药品"


def normalize_short_diagnosis(value: str) -> str:
    """把模型诊断压缩成适合 t16 显示的单行短文本。"""
    first_line = value.replace("\r", "\n").split("\n", 1)[0].strip()
    first_line = first_line.replace("|", " ").replace("--", " ")
    first_line = first_line.replace("建议服用", " ")
    first_line = first_line.strip(" \t。.:：,，;；\"'“”")
    if not first_line:
        return "无法判断"
    return first_line[:24]


def eligible_health_inventory(records: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    """Return in-stock medicines with a known, non-expired shelf life."""

    eligible: List[Dict[str, Any]] = []
    for record in records:
        name = str(record.get("name", "")).strip()
        try:
            quantity = int(record.get("quantity", 0))
        except (TypeError, ValueError):
            continue
        if (
            not record.get("valid")
            or quantity <= 0
            or not name
            or "|" in name
            or "--" in name
            or "\n" in name
            or "\r" in name
        ):
            continue

        expiry = medicine_expiry(
            str(record.get("production_date", "")),
            str(record.get("shelf_life", "")),
        )
        if expiry.get("expiry_status") not in {"normal", "expiring"}:
            continue
        eligible.append(record)
    return eligible


_EMERGENCY_SYMPTOM_TERMS = (
    "呼吸困难",
    "喘不上气",
    "无法呼吸",
    "不能呼吸",
    "没有呼吸",
    "无呼吸",
    "呼吸停止",
    "心跳停止",
    "意识不清",
    "失去意识",
    "没有意识",
    "无意识",
    "昏迷",
    "晕厥",
    "抽搐",
    "胸口剧痛",
    "心前区痛",
    "心脏疼",
    "心脏痛",
    "心口疼",
    "心口痛",
    "胸口疼",
    "胸疼",
    "胸痛",
    "大出血",
    "出血不止",
    "大量便血",
    "严重便血",
    "吐了血",
    "吐出血",
    "吐血",
    "呕血",
    "咳血",
    "咯血",
    "黑便",
    "半身不遂",
    "单侧无力",
    "一侧无力",
    "嘴歪",
    "言语不清",
    "剧烈头痛",
    "过敏性休克",
    "严重过敏",
    "中毒",
    "自杀",
    "自伤",
)
_EMERGENCY_NEGATION_PREFIXES = (
    "没有",
    "并无",
    "未见",
    "未出现",
    "否认",
    "不是",
    "不想",
    "无",
    "未",
    "不",
)
_CARE_FORBIDDEN_TERMS = (
    "mg",
    "ml",
    "毫克",
    "毫升",
    "剂量",
    "疗程",
    "停药",
    "加量",
    "减量",
    "服药",
    "吃药",
    "用药",
    "服用",
    "片",
    "粒",
    "丸",
    "袋",
    "次",
)


def health_symptom_is_emergency(symptom: str) -> bool:
    """Match explicit high-risk symptoms without relying on the AI service."""

    compact = "".join(str(symptom or "").split())
    if not compact:
        return False

    for term in _EMERGENCY_SYMPTOM_TERMS:
        offset = 0
        while True:
            index = compact.find(term, offset)
            if index < 0:
                break
            prefix = compact[max(0, index - 4):index]
            if not any(prefix.endswith(value) for value in _EMERGENCY_NEGATION_PREFIXES):
                return True
            offset = index + len(term)
    return False


def health_symptom_forces_no_medicine(symptom: str) -> bool:
    """Reject an empty or too-short symptom without asking the model."""

    compact = "".join(str(symptom or "").split())
    return len(compact) < 2


def emergency_health_assessment() -> HealthAssessment:
    return HealthAssessment(
        HealthStatus.EMERGENCY,
        HEALTH_EMERGENCY_DIAGNOSIS,
        VOICE_EMERGENCY_REPLY,
    )


def no_medicine_health_assessment() -> HealthAssessment:
    return HealthAssessment(
        HealthStatus.NO_MEDICINE,
        "无法判断",
        HEALTH_NO_MEDICINE,
    )


def fallback_health_care_phrase(symptom: str, diagnosis: str) -> str:
    """Return a short, non-medication fallback related to the current symptom."""

    context = "".join(f"{symptom}{diagnosis}".split())
    if any(term in context for term in ("发烧", "发热", "感冒", "咳嗽", "嗓子", "咽")):
        return "记得多喝温水哦"
    if any(term in context for term in ("头痛", "头疼", "乏力", "疲劳", "失眠", "熬夜")):
        return "注意休息别熬夜"
    if any(term in context for term in ("胃", "肠", "腹泻", "拉肚子", "恶心", "呕吐")):
        return "饮食清淡些哦"
    return "照顾好自己哦"


def normalize_health_care_phrase(value: str, symptom: str, diagnosis: str) -> str:
    """Validate an AI-generated care phrase or replace it with a safe fallback."""

    raw = str(value or "")
    compact = "".join(raw.split())
    lowered = compact.lower()
    invalid = (
        any(ord(ch) < 0x20 or ord(ch) == 0x7F for ch in raw)
        or "|" in compact
        or "--" in compact
        or any(ch.isdigit() for ch in compact)
        or not (2 <= len(compact) <= HEALTH_CARE_MAX_CHARS)
        or any(term in lowered for term in _CARE_FORBIDDEN_TERMS)
    )
    if invalid:
        return fallback_health_care_phrase(symptom, diagnosis)
    return compact


def select_inventory_medicine(value: str, records: List[Dict[str, Any]]) -> str:
    """Only accept one exact eligible inventory name from model output."""
    value = value.strip(" \t\r\n。.:：,，;；\"'“”")
    if "无对应药品" in value:
        return HEALTH_NO_MEDICINE

    medicine_names = [
        str(record.get("name", "")).strip()
        for record in eligible_health_inventory(records)
    ]
    for name in medicine_names:
        if value == name:
            return name
    return HEALTH_NO_MEDICINE


async def ask_health_assessment(client: AsyncOpenAI,
                                 symptom: str,
                                 records: List[Dict[str, Any]]) -> HealthAssessment:
    """Return one validated normal, no-medicine, or emergency assessment."""
    if health_symptom_is_emergency(symptom):
        return emergency_health_assessment()
    if health_symptom_forces_no_medicine(symptom):
        return no_medicine_health_assessment()
    valid_records = eligible_health_inventory(records)

    medicine_lines = [
        f"{record.get('name', '')}: {record.get('efficacy', '') or MEDICINE_UNKNOWN}"
        for record in valid_records
    ]
    inventory_text = "\n".join(medicine_lines) if medicine_lines else "（当前库存为空）"

    response = await client.chat.completions.create(
        model=MODEL,
        messages=[
            {
                "role": "system",
                "content": (
                    "你是智能药箱的健康初筛助手。先判断症状属于普通、无药或急症。"
                    "胸痛、呼吸困难、呕血、意识障碍、大出血等可能危及生命的情况必须写急症。"
                    "非急症时给出一个简短的初步判断，并且只能从用户当前库存药品中"
                    "选择一个适合的药品。"
                    "初步判断只能表示可能性，必须简短，不能作为确定诊断。"
                    "不要给用法、用量、疗程或长篇解释。"
                    "急症时状态写“急症”，其余字段内容不会被采用。"
                    "若信息不足、症状含糊或不适合自行选药，状态写“无药”，"
                    "诊断写“无法判断”，药品写“无对应药品”；"
                    "若库存为空或没有合适药品，药品写“无对应药品”。"
                    "每次最多选择一个药品，绝对不能推荐库存清单以外的药品。"
                    "只有状态为“普通”且选中了有效库存药品时，生成一句结合症状的生活关怀短句。"
                    "关怀短句必须为2到10个字，不得包含诊断、药名、剂量、疗程或用药指令。"
                    "输出必须只有一行，严格使用“状态|诊断|药品名|关心话”格式，"
                    "状态只能是“普通”“无药”“急症”。"
                ),
            },
            {
                "role": "user",
                "content": (
                    f"症状：{symptom}\n"
                    "当前库存药品及药效：\n"
                    f"{inventory_text}\n"
                    "请只返回：状态|诊断|药品名|关心话"
                ),
            },
        ],
        stream=False,
        temperature=0.0,
    )

    content = (response.choices[0].message.content or "").strip()
    first_line = content.splitlines()[0].strip() if content else ""
    parts = first_line.split("|")
    if len(parts) != 4:
        return no_medicine_health_assessment()

    status_text, diagnosis_text, medicine_text, care_text = (
        part.strip() for part in parts
    )
    if status_text.lower() in {"急症", "emergency"}:
        return emergency_health_assessment()
    if status_text.lower() not in {"普通", "normal"}:
        return no_medicine_health_assessment()

    diagnosis = normalize_short_diagnosis(diagnosis_text)
    medicine = select_inventory_medicine(medicine_text, valid_records)
    if diagnosis == "无法判断":
        return no_medicine_health_assessment()
    if medicine == HEALTH_NO_MEDICINE:
        return no_medicine_health_assessment()

    care_phrase = normalize_health_care_phrase(care_text, symptom, diagnosis)
    return HealthAssessment(
        HealthStatus.NORMAL,
        diagnosis,
        medicine,
        care_phrase,
    )


def format_voice_health_reply(assessment: HealthAssessment) -> str:
    """Build the one permitted spoken response for the validated status."""

    if assessment.status == HealthStatus.EMERGENCY:
        return VOICE_EMERGENCY_REPLY
    if assessment.status != HealthStatus.NORMAL:
        return VOICE_NO_MEDICINE_REPLY
    safe_diagnosis = normalize_short_diagnosis(assessment.diagnosis)
    if safe_diagnosis == "无法判断":
        return VOICE_NO_MEDICINE_REPLY
    care_phrase = normalize_health_care_phrase(
        assessment.care_phrase,
        safe_diagnosis,
        safe_diagnosis,
    )
    return (
        f"可能是--{safe_diagnosis}，建议服用--{assessment.medicine}，"
        f"{care_phrase}"
    )


async def handle_health_command(user_text: str, client: AsyncOpenAI) -> str:
    """处理 HA ASSESS UTF-8 症状文本请求。"""
    rest = user_text[3:].strip()
    op, _, payload = rest.partition(" ")
    if op.upper() != "ASSESS":
        return health_err("unsupported command")

    started_at = time.perf_counter()
    try:
        symptom = validate_symptom_field(payload)
        async with medicine_lock:
            records = load_medicine_records()
        assessment = await ask_health_assessment(client, symptom, records)
        diagnosis = assessment.diagnosis
        medicine = assessment.medicine
        latency_ms = (time.perf_counter() - started_at) * 1000.0
        dashboard_health(symptom, diagnosis, medicine, True, latency_ms)
        dashboard_event("ai", "success", f"AI初筛：{diagnosis}", f"库存推荐：{medicine}")
        return health_ok(diagnosis, medicine)
    except ValueError as exc:
        dashboard_health(payload, "症状无效", "无对应药品", False,
                         (time.perf_counter() - started_at) * 1000.0)
        dashboard_event("ai", "warning", "健康助手请求无效", str(exc))
        return health_err(str(exc))
    except Exception:
        logger.exception("health assessment failed")
        dashboard_health(payload, "诊断失败", "无对应药品", False,
                         (time.perf_counter() - started_at) * 1000.0)
        dashboard_event("ai", "danger", "健康助手请求失败", "DeepSeek调用异常")
        return health_err("AI request failed")


async def handle_medicine_command(user_text: str, client: AsyncOpenAI) -> str:
    """处理药瓶仓储命令；RECOMMEND 会读取本地药箱后调用 DeepSeek。"""
    op, payload = parse_medicine_operation(user_text)
    if not op:
        return medicine_err("missing command")

    try:
        if op == "RECOMMEND":
            symptom = validate_symptom_field(payload)
            async with medicine_lock:
                records = load_medicine_records()

            recommendation = await ask_medicine_recommendation(client, symptom, records)
            return medicine_ok(f"RECOMMEND {recommendation}")

        async with medicine_lock:
            records = load_medicine_records()

            if op == "GET":
                slot = parse_slot(payload)
                record = records[slot]
                if not record.get("valid"):
                    return medicine_err("EMPTY")
                return medicine_ok(f"GET {format_medicine_record(record)}")

            if op == "GETQ":
                slot = parse_slot(payload)
                record = records[slot]
                if not record.get("valid"):
                    return medicine_err("EMPTY")
                return medicine_ok(f"GETQ {format_medicine_record_with_quantity(record)}")

            if op == "SET":
                fields = split_medicine_fields_range(payload, 5, 6)
                slot_text, name, production_date, shelf_life, efficacy = fields[:5]
                slot = parse_slot(slot_text)
                previous_quantity = int(records[slot].get("quantity", 1)) if records[slot].get("valid") else 1
                quantity = parse_quantity(fields[5]) if len(fields) == 6 else previous_quantity
                records[slot] = {
                    "valid": True,
                    "slot": slot,
                    "quantity": quantity,
                    "name": validate_medicine_field("name", name),
                    "production_date": validate_medicine_field("production_date", production_date),
                    "shelf_life": validate_medicine_field("shelf_life", shelf_life),
                    "efficacy": validate_medicine_field("efficacy", efficacy),
                }
                save_medicine_records(records)
                logger.info("medicine SET slot=%s name=%s", slot, records[slot].get("name", ""))
                return medicine_ok(f"SET {slot}")

            if op == "ADD":
                fields = split_medicine_fields_range(payload, 4, 5)
                name, production_date, shelf_life, efficacy = fields[:4]
                quantity = parse_quantity(fields[4]) if len(fields) == 5 else 1
                empty_slots = [record["slot"] for record in records if not record.get("valid")]
                if not empty_slots:
                    return medicine_err("FULL")

                slot = int(empty_slots[0])
                records[slot] = {
                    "valid": True,
                    "slot": slot,
                    "quantity": quantity,
                    "name": validate_medicine_field("name", name),
                    "production_date": validate_medicine_field("production_date", production_date),
                    "shelf_life": validate_medicine_field("shelf_life", shelf_life),
                    "efficacy": validate_medicine_field("efficacy", efficacy),
                }
                save_medicine_records(records)
                logger.info("medicine ADD slot=%s name=%s", slot, records[slot].get("name", ""))
                return medicine_ok(f"ADD {slot}")

            if op == "DEL":
                slot = parse_slot(payload)
                old_name = records[slot].get("name", "")
                records[slot] = make_blank_medicine_record(slot)
                save_medicine_records(records)
                logger.info("medicine DEL slot=%s old_name=%s", slot, old_name)
                return medicine_ok(f"DEL {slot}")

            if op in {"QTY", "RESTOCK", "TAKE"}:
                slot_text, quantity_text = split_medicine_fields(payload, 2)
                slot = parse_slot(slot_text)
                quantity = parse_quantity(quantity_text, allow_zero=(op == "QTY"))
                new_quantity = apply_medicine_quantity(records, slot, op, quantity)
                save_medicine_records(records)
                logger.info("medicine %s slot=%s quantity=%s", op, slot, new_quantity)
                return medicine_ok(f"{op} {slot}|{new_quantity}")

            if op == "LIST":
                items = [
                    f"{record['slot']}:{record.get('name', '')}"
                    for record in records
                    if record.get("valid")
                ]
                return medicine_ok("LIST " + (";".join(items) if items else "EMPTY"))

            if op == "LISTFULL":
                items = [
                    format_medicine_inventory_record(record)
                    for record in records
                    if record.get("valid") and int(record.get("quantity", 0)) > 0
                ]
                return medicine_ok("LISTFULL " + ("||".join(items) if items else "EMPTY"))

            return medicine_err("unknown command")
    except ValueError as exc:
        return medicine_err(str(exc))
    except Exception as exc:
        logger.exception("medicine command failed: %s", user_text[:120])
        return medicine_err(str(exc))


def make_client() -> AsyncOpenAI:
    """创建 DeepSeek API 客户端。

    DeepSeek 兼容 OpenAI SDK，所以这里使用 openai.AsyncOpenAI。
    api_key 必须从环境变量读取，缺少时直接报错，避免程序悄悄以错误配置运行。
    """
    api_key = os.getenv("DEEPSEEK_API_KEY")
    if not api_key:
        raise RuntimeError("DEEPSEEK_API_KEY is not set")

    return AsyncOpenAI(api_key=api_key, base_url=BASE_URL, timeout=REQUEST_TIMEOUT_SECONDS)


def trim_history(history: Deque[Dict[str, str]]) -> List[Dict[str, str]]:
    """裁剪对话历史，避免上下文无限增长。

    单片机串口链路不适合传太长内容，服务器也不应该把历史越攒越多。
    这里保留最近 MAX_HISTORY_PAIRS 轮，然后在最前面加 system prompt。
    """
    max_items = MAX_HISTORY_PAIRS * 2
    while len(history) > max_items:
        history.popleft()
    return [{"role": "system", "content": SYSTEM_PROMPT}, *history]


async def ask_deepseek(client: AsyncOpenAI, history: Deque[Dict[str, str]], user_text: str) -> str:
    """把用户文字发给 DeepSeek，并返回模型回复。

    这里特别注意：
    - 使用 model=deepseek-v4-pro。
    - stream=False，服务器一次性拿完整回复再发回单片机。
    - 不传 reasoning_effort。
    - 不传 extra_body={"thinking": ...}。

    这样就是你要求的“非思考模式”。
    """
    history.append({"role": "user", "content": user_text})
    messages = trim_history(history)

    # Non-thinking mode: do not pass reasoning_effort or extra_body thinking options.
    response = await client.chat.completions.create(
        model=MODEL,
        messages=messages,
        stream=False,
    )

    content = response.choices[0].message.content or ""
    content = content.strip() or "DeepSeek 返回了空内容。"
    history.append({"role": "assistant", "content": content})
    trim_history(history)
    return content


async def send_text(writer: asyncio.StreamWriter, text: str) -> None:
    """向单片机/W800 发送文本。

    串口工具一般喜欢 CRLF 换行，也就是 \\r\\n。
    这里统一把 Python 字符串里的换行转成 \\r\\n，显示会更整齐。
    """
    normalized = text.replace("\r\n", "\n").replace("\r", "\n").replace("\n", "\r\n")
    writer.write(normalized.encode("utf-8", errors="replace"))
    await writer.drain()


def make_json_body(payload: Dict[str, Any]) -> bytes:
    return json.dumps(payload, ensure_ascii=False).encode("utf-8")


async def send_http_bytes(writer: asyncio.StreamWriter,
                          status: int,
                          body: bytes,
                          content_type: str,
                          *,
                          cache_control: str = "no-store",
                          extra_headers: Dict[str, str] | None = None) -> None:
    reason = {
        200: "OK",
        201: "Created",
        204: "No Content",
        400: "Bad Request",
        401: "Unauthorized",
        403: "Forbidden",
        404: "Not Found",
        405: "Method Not Allowed",
        409: "Conflict",
        413: "Content Too Large",
        423: "Locked",
        429: "Too Many Requests",
        500: "Internal Server Error",
        503: "Service Unavailable",
    }.get(status, "OK")
    if status == 204:
        body = b""
    headers = [
        f"HTTP/1.1 {status} {reason}",
        f"Content-Type: {content_type}",
        f"Content-Length: {len(body)}",
        "Connection: close",
        f"Cache-Control: {cache_control}",
        "Access-Control-Allow-Origin: *",
        "Access-Control-Allow-Headers: Content-Type, X-Fridge-Token, X-CSRF-Token",
        "Access-Control-Allow-Methods: GET, POST, PUT, PATCH, DELETE, OPTIONS",
        "X-Content-Type-Options: nosniff",
        "X-Frame-Options: DENY",
        "Referrer-Policy: no-referrer",
        "Content-Security-Policy: default-src 'self'; connect-src 'self'; img-src 'self' data:; style-src 'self'; script-src 'self'; base-uri 'none'; frame-ancestors 'none'",
    ]
    for key, value in (extra_headers or {}).items():
        safe_key = str(key).replace("\r", "").replace("\n", "")
        safe_value = str(value).replace("\r", "").replace("\n", "")
        headers.append(f"{safe_key}: {safe_value}")
    headers.extend(["", ""])
    writer.write("\r\n".join(headers).encode("ascii") + body)
    await writer.drain()


async def send_http_json(writer: asyncio.StreamWriter,
                         status: int,
                         payload: Dict[str, Any],
                         *,
                         extra_headers: Dict[str, str] | None = None) -> None:
    await send_http_bytes(
        writer,
        status,
        make_json_body(payload) if status != 204 else b"",
        "application/json; charset=utf-8",
        extra_headers=extra_headers,
    )


def safe_firmware_filename(value: str) -> str:
    decoded = unquote(str(value or "")).strip()
    filename = Path(decoded).name
    if not filename or filename != decoded or any(char in filename for char in ("/", "\\", "\x00")):
        raise ValueError("固件文件名无效")
    if len(filename) > 160:
        raise ValueError("固件文件名过长")
    if Path(filename).suffix.lower() not in FIRMWARE_ALLOWED_SUFFIXES:
        raise ValueError("不支持的固件文件类型")
    return filename


def remove_unreferenced_firmware_files(artifacts: List[Dict[str, Any]]) -> None:
    root = FIRMWARE_ROOT.resolve()
    for artifact in artifacts:
        if int(artifact.get("remaining_references", 1)) != 0:
            continue
        stored_name = str(artifact.get("stored_name") or "")
        candidate = (root / stored_name).resolve()
        if candidate.parent == root:
            with contextlib.suppress(FileNotFoundError, OSError):
                candidate.unlink()


async def send_firmware_download(method: str,
                                 target: str,
                                 writer: asyncio.StreamWriter) -> bool:
    path = unquote(urlparse(target).path)
    match = re.fullmatch(r"/api/public/firmware/artifacts/(\d+)/download", path)
    if match is None:
        return False
    if method != "GET":
        await send_http_json(writer, 405, {"ok": False, "error": "method not allowed"})
        return True
    if dashboard_store is None:
        await send_http_json(writer, 503, {"ok": False, "error": "dashboard is starting"})
        return True
    artifact = dashboard_store.firmware_artifact(int(match.group(1)))
    if artifact is None or not bool(artifact.get("published")):
        await send_http_json(writer, 404, {"ok": False, "error": "固件文件不存在"})
        return True
    root = FIRMWARE_ROOT.resolve()
    file_path = (root / str(artifact["stored_name"])).resolve()
    if file_path.parent != root or not file_path.is_file():
        await send_http_json(writer, 404, {"ok": False, "error": "固件文件不存在"})
        return True
    size = file_path.stat().st_size
    filename = str(artifact["filename"]).replace("\r", "").replace("\n", "")
    content_type = mimetypes.guess_type(filename)[0] or "application/octet-stream"
    headers = [
        "HTTP/1.1 200 OK",
        f"Content-Type: {content_type}",
        f"Content-Length: {size}",
        f"Content-Disposition: attachment; filename*=UTF-8''{quote(filename)}",
        "Cache-Control: public, max-age=300",
        "Connection: close",
        "X-Content-Type-Options: nosniff",
        "",
        "",
    ]
    writer.write("\r\n".join(headers).encode("ascii"))
    await writer.drain()
    with file_path.open("rb") as stream:
        while True:
            chunk = stream.read(64 * 1024)
            if not chunk:
                break
            writer.write(chunk)
            await writer.drain()
    dashboard_store.increment_firmware_download(int(artifact["id"]))
    return True


async def receive_firmware_upload(reader: asyncio.StreamReader,
                                  target: str,
                                  headers: Dict[str, str],
                                  content_length: int) -> tuple[int, Dict[str, Any]]:
    if dashboard_store is None:
        return 503, {"ok": False, "error": "dashboard is starting"}
    auth, auth_error = authenticate_http_request("POST", headers)
    if auth is None or auth.get("type") != "admin":
        return 403 if auth_error in {"csrf", "origin", "client_disabled"} else 401, {
            "ok": False,
            "error": auth_error or "admin required",
        }
    if content_length <= 0 or content_length > MAX_FIRMWARE_UPLOAD_BYTES:
        return 413, {"ok": False, "error": "固件文件大小必须在1字节到256MB之间"}
    if headers.get("content-type", "").split(";", 1)[0].strip().lower() != "application/octet-stream":
        return 400, {"ok": False, "error": "固件上传必须使用application/octet-stream"}
    query = parse_qs(urlparse(target).query)
    release_id = int((query.get("release_id") or ["0"])[0])
    kind = (query.get("kind") or [""])[0].strip().lower()
    filename = safe_firmware_filename((query.get("filename") or [""])[0])
    target_part = (query.get("target_part") or [OTA_TARGET_PART])[0].strip()
    min_bootloader = (query.get("min_bootloader") or ["1.0.0+0"])[0].strip()
    release = dashboard_store.firmware_release(release_id)
    if release is None:
        return 404, {"ok": False, "error": "固件版本不存在"}
    if kind not in FIRMWARE_KINDS:
        return 400, {"ok": False, "error": "固件文件用途无效"}

    FIRMWARE_ROOT.mkdir(parents=True, exist_ok=True)
    root = FIRMWARE_ROOT.resolve()
    temp_path = root / f".upload-{secrets.token_hex(16)}.tmp"
    digest = hashlib.sha256()
    remaining = content_length
    try:
        with temp_path.open("wb") as stream:
            while remaining:
                chunk = await reader.readexactly(min(64 * 1024, remaining))
                stream.write(chunk)
                digest.update(chunk)
                remaining -= len(chunk)
        sha256 = digest.hexdigest()
        inspection = None
        if kind == "ota":
            if release.get("component") != "main":
                return 400, {"ok": False, "error": "OTA镜像只能属于RA6M5主控版本"}
            if Path(filename).suffix.lower() != ".bin":
                return 400, {"ok": False, "error": "OTA镜像必须是MCUboot签名的.bin文件"}
            if target_part != OTA_TARGET_PART:
                return 400, {"ok": False, "error": "OTA目标芯片与服务器配置不一致"}
            try:
                expected_version = normalize_version(str(release.get("version", "")))
                normalized_min_bootloader = normalize_version(min_bootloader)
                inspection = inspect_mcuboot_image(temp_path, OTA_PUBLIC_KEY_PATH)
            except FirmwareImageError as exc:
                failed = failed_inspection(temp_path, exc)
                return 400, {
                    "ok": False,
                    "error": "MCUboot镜像验证失败",
                    "validation_error": failed.validation_error,
                }
            if inspection.image_version != expected_version:
                return 400, {
                    "ok": False,
                    "error": "网站版本号与MCUboot镜像版本不一致",
                    "image_version": inspection.image_version,
                    "release_version": expected_version,
                }
            if inspection.signed_size != content_length or inspection.sha256 != sha256:
                return 400, {"ok": False, "error": "OTA镜像长度或SHA-256检查失败"}
        else:
            normalized_min_bootloader = ""

        suffix = Path(filename).suffix.lower()
        stored_name = f"{sha256}{suffix}"
        final_path = root / stored_name
        deduplicated = final_path.exists()
        if deduplicated:
            temp_path.unlink()
        else:
            os.replace(temp_path, final_path)
        for existing in release.get("artifacts", []):
            if existing.get("kind") == kind and existing.get("sha256") == sha256:
                return 200, {"ok": True, "artifact": existing, "deduplicated": True}
        artifact = dashboard_store.add_firmware_artifact(
            release_id,
            kind,
            filename,
            stored_name,
            content_length,
            sha256,
            target_part=target_part if inspection is not None else "",
            image_version=inspection.image_version if inspection is not None else "",
            min_bootloader=normalized_min_bootloader,
            signature_type=inspection.signature_type if inspection is not None else "",
            image_valid=bool(inspection and inspection.valid),
            validation_error=inspection.validation_error if inspection is not None else "",
        )
        dashboard_audit(
            str(auth["actor"]),
            "firmware",
            "artifact_upload",
            target=f"release:{release_id}",
            summary=f"{filename} {content_length}B",
            result="success",
        )
        logger.info("firmware upload release=%s file=%s bytes=%s", release_id, filename, content_length)
        return 201, {"ok": True, "artifact": artifact, "deduplicated": deduplicated}
    finally:
        with contextlib.suppress(FileNotFoundError, OSError):
            temp_path.unlink()


STATIC_DASHBOARD_FILES = {
    "/": ("index.html", "text/html; charset=utf-8"),
    "/index.html": ("index.html", "text/html; charset=utf-8"),
    "/monitor": ("index.html", "text/html; charset=utf-8"),
    "/inventory": ("index.html", "text/html; charset=utf-8"),
    "/firmware": ("index.html", "text/html; charset=utf-8"),
    "/dashboard.css": ("dashboard.css", "text/css; charset=utf-8"),
    "/dashboard.js": ("dashboard.js", "application/javascript; charset=utf-8"),
    "/admin": ("admin.html", "text/html; charset=utf-8"),
    "/admin.html": ("admin.html", "text/html; charset=utf-8"),
    "/admin/overview": ("admin.html", "text/html; charset=utf-8"),
    "/admin/devices": ("admin.html", "text/html; charset=utf-8"),
    "/admin/monitor": ("admin.html", "text/html; charset=utf-8"),
    "/admin/inventory": ("admin.html", "text/html; charset=utf-8"),
    "/admin/logs": ("admin.html", "text/html; charset=utf-8"),
    "/admin/firmware": ("admin.html", "text/html; charset=utf-8"),
    "/admin/ai": ("admin.html", "text/html; charset=utf-8"),
    "/admin.css": ("admin.css", "text/css; charset=utf-8"),
    "/admin.js": ("admin.js", "application/javascript; charset=utf-8"),
}


async def send_dashboard_asset(method: str, target: str, writer: asyncio.StreamWriter) -> bool:
    """Serve only fixed dashboard assets; never map arbitrary paths."""
    path = unquote(urlparse(target).path)
    asset = STATIC_DASHBOARD_FILES.get(path)
    if asset is None:
        return False
    if method != "GET":
        await send_http_json(writer, 405, {"ok": False, "error": "method not allowed"})
        return True

    filename, content_type = asset
    file_path = DASHBOARD_WEB_ROOT / filename
    try:
        body = file_path.read_bytes()
    except FileNotFoundError:
        await send_http_json(writer, 404, {"ok": False, "error": "dashboard asset not found"})
        return True
    await send_http_bytes(writer, 200, body, content_type, cache_control="no-cache")
    return True


def decode_http_headers(raw_headers: bytes) -> tuple[str, str, Dict[str, str]]:
    text = raw_headers.decode("iso-8859-1", errors="replace")
    lines = text.split("\r\n")
    method, target, _version = lines[0].split(" ", 2)
    headers: Dict[str, str] = {}
    for line in lines[1:]:
        if not line or ":" not in line:
            continue
        key, value = line.split(":", 1)
        headers[key.strip().lower()] = value.strip()
    return method.upper(), target, headers


def parse_json_request(body: bytes) -> Dict[str, Any]:
    if not body:
        return {}
    payload = json.loads(body.decode("utf-8"))
    if not isinstance(payload, dict):
        raise ValueError("JSON body must be an object")
    return payload


def json_field(payload: Dict[str, Any], name: str, default: str = "") -> str:
    value = payload.get(name, default)
    return "" if value is None else str(value)


async def handle_public_dashboard_api(method: str,
                                      path: str,
                                      query: Dict[str, List[str]]) -> tuple[int, Dict[str, Any]]:
    """Read-only public dashboard endpoints; no command or secret is exposed here."""
    if method != "GET":
        return 405, {"ok": False, "error": "method not allowed"}
    if dashboard_store is None:
        return 503, {"ok": False, "error": "dashboard is starting"}

    if path == "/api/public/dashboard/overview":
        async with fridge_lock:
            status = fridge_snapshot_unlocked()
            control_pending = fridge_control_dirty
        async with medicine_lock:
            records = load_medicine_records()
        return 200, {
            "ok": True,
            **dashboard_store.overview(status, records, control_pending=control_pending),
        }

    if path == "/api/public/dashboard/history":
        range_key = (query.get("range") or ["1h"])[0]
        return 200, {"ok": True, **dashboard_store.history(range_key)}

    if path == "/api/public/dashboard/inventory":
        async with medicine_lock:
            records = load_medicine_records()
        return 200, {"ok": True, **dashboard_store.inventory(records)}

    if path == "/api/public/dashboard/events":
        limit = int((query.get("limit") or ["50"])[0])
        return 200, {"ok": True, "events": dashboard_store.events(limit)}

    if path == "/api/public/dashboard/health":
        limit = int((query.get("limit") or ["10"])[0])
        return 200, {"ok": True, "records": dashboard_store.health(limit)}

    if path == "/api/public/firmware/releases":
        return 200, {"ok": True, "releases": dashboard_store.firmware_releases()}

    return 404, {"ok": False, "error": "not found"}


async def handle_http_api(method: str,
                          target: str,
                          headers: Dict[str, str],
                          body: bytes,
                          *,
                          peer_ip: str = "unknown",
                          response_headers: Dict[str, str] | None = None) -> tuple[int, Dict[str, Any]]:
    global fridge_control_dirty, pending_control_command, pending_door_command
    global last_control_command

    parsed = urlparse(target)
    path = unquote(parsed.path)
    query = parse_qs(parsed.query)

    if method == "OPTIONS":
        return 204, {}

    try:
        if path.startswith("/api/public/"):
            return await handle_public_dashboard_api(method, path, query)

        if path == "/api/admin/login":
            if method != "POST":
                return 405, {"ok": False, "error": "method not allowed"}
            if not ADMIN_PASSWORD_HASH:
                return 503, {"ok": False, "error": "管理员密码尚未配置"}
            if not ALLOW_INSECURE_ADMIN and not ADMIN_COOKIE_SECURE:
                return 503, {"ok": False, "error": "HTTP管理登录未启用"}
            allowed, retry_after = login_rate_state(peer_ip)
            if not allowed:
                if response_headers is not None:
                    response_headers["Retry-After"] = str(retry_after)
                return 429, {"ok": False, "error": "登录尝试过多，请稍后再试", "retry_after": retry_after}
            payload = parse_json_request(body)
            password = str(payload.get("password") or "")
            if not verify_admin_password(password):
                record_login_failure(peer_ip)
                dashboard_audit("anonymous", "security", "admin_login", result="denied")
                return 401, {"ok": False, "error": "管理员密码错误"}
            if dashboard_store is None:
                return 503, {"ok": False, "error": "dashboard is starting"}
            clear_login_failures(peer_ip)
            session_token = secrets.token_urlsafe(32)
            csrf_token = secrets.token_urlsafe(24)
            expires_at = time.time() + ADMIN_SESSION_TTL_SECONDS
            dashboard_store.create_admin_session(
                session_token_hash(session_token),
                csrf_token,
                expires_at,
            )
            if response_headers is not None:
                response_headers["Set-Cookie"] = admin_cookie_header(session_token)
            dashboard_audit("web_admin", "security", "admin_login", result="success")
            return 200, {
                "ok": True,
                "authenticated": True,
                "csrf_token": csrf_token,
                "expires_at": expires_at,
                "insecure_http": not ADMIN_COOKIE_SECURE,
            }

        if path == "/api/admin/session":
            if method != "GET":
                return 405, {"ok": False, "error": "method not allowed"}
            session = admin_session_from_headers(headers)
            if session is None:
                return 401, {"ok": False, "authenticated": False, "error": "unauthorized"}
            return 200, {
                "ok": True,
                "authenticated": True,
                "csrf_token": session["csrf_token"],
                "expires_at": session["expires_at"],
                "insecure_http": not ADMIN_COOKIE_SECURE,
            }

        auth, auth_error = authenticate_http_request(method, headers)
        if auth is None:
            status = 403 if auth_error in {"csrf", "origin", "client_disabled"} else 401
            return status, {"ok": False, "error": auth_error or "unauthorized"}
        actor = str(auth["actor"])

        if path == "/api/admin/logout":
            if method != "POST" or auth["type"] != "admin":
                return 405 if method != "POST" else 403, {"ok": False, "error": "method not allowed"}
            token = cookie_value(headers, ADMIN_SESSION_COOKIE)
            if dashboard_store is not None and token:
                dashboard_store.delete_admin_session(session_token_hash(token))
            if response_headers is not None:
                response_headers["Set-Cookie"] = admin_cookie_header("", clear=True)
            dashboard_audit(actor, "security", "admin_logout", result="success")
            return 200, {"ok": True}

        if path == "/api/admin/bootstrap":
            if method != "GET" or auth["type"] != "admin":
                return 405 if method != "GET" else 403, {"ok": False, "error": "method not allowed"}
            async with fridge_lock:
                refresh_control_commands_unlocked()
                return 200, {
                    "ok": True,
                    "status": fridge_snapshot_unlocked(),
                    "control": dict(fridge_control),
                    "control_pending": fridge_control_dirty,
                    "command": public_command(last_control_command),
                    "clients": dashboard_store.client_summary(),
                    "power_settings": dashboard_store.power_settings(),
                    "power_alerts": dashboard_store.power_alerts(5, active_only=True),
                    "recognitions": dashboard_store.recent_vision_recognitions(5),
                    "capabilities": {
                        "set_temp": {"min": -40, "max": 80, "step": 1},
                        "fan_modes": [0, 1, 2],
                        "medicine_slots": MEDICINE_SLOT_COUNT,
                        "quantity_max": MEDICINE_QUANTITY_MAX,
                    },
                    "insecure_http": not ADMIN_COOKIE_SECURE,
                }

        if path == "/api/admin/recognitions":
            if method != "GET" or auth["type"] != "admin":
                return 405 if method != "GET" else 403, {"ok": False, "error": "method not allowed"}
            limit = int((query.get("limit") or ["5"])[0])
            return 200, {
                "ok": True,
                "records": dashboard_store.recent_vision_recognitions(limit),
            }

        if path == "/api/admin/audit":
            if method != "GET" or auth["type"] != "admin":
                return 405 if method != "GET" else 403, {"ok": False, "error": "method not allowed"}
            limit = int((query.get("limit") or ["100"])[0])
            return 200, {
                "ok": True,
                "records": dashboard_store.audit(limit) if dashboard_store is not None else [],
            }

        if path == "/api/admin/clients":
            if auth["type"] != "admin":
                return 403, {"ok": False, "error": "admin required"}
            if method == "GET":
                return 200, {
                    "ok": True,
                    "summary": dashboard_store.client_summary(),
                    "clients": dashboard_store.clients(),
                }
            if method == "POST":
                payload = parse_json_request(body)
                name = json_field(payload, "name").strip()
                if not name:
                    return 400, {"ok": False, "error": "客户端名称不能为空"}
                token = make_app_client_token()
                client_record = dashboard_store.create_client(
                    name,
                    session_token_hash(token),
                    app_token_prefix(token),
                    json_field(payload, "notes"),
                )
                dashboard_audit(actor, "security", "client_create", target=f"client:{client_record['id']}", summary=name, result="success")
                return 201, {"ok": True, "client": client_record, "token": token}
            return 405, {"ok": False, "error": "method not allowed"}

        if path.startswith("/api/admin/clients/"):
            if auth["type"] != "admin":
                return 403, {"ok": False, "error": "admin required"}
            parts = path.removeprefix("/api/admin/clients/").split("/")
            client_id = int(parts[0])
            if len(parts) == 2 and parts[1] == "regenerate-token":
                if method != "POST":
                    return 405, {"ok": False, "error": "method not allowed"}
                token = make_app_client_token()
                client_record = dashboard_store.replace_client_token(
                    client_id,
                    session_token_hash(token),
                    app_token_prefix(token),
                )
                if client_record is None:
                    return 404, {"ok": False, "error": "客户端不存在或共享客户端不能重置"}
                dashboard_audit(actor, "security", "client_token_regenerate", target=f"client:{client_id}", result="success")
                return 200, {"ok": True, "client": client_record, "token": token}
            if len(parts) != 1 or method != "PATCH":
                return 405, {"ok": False, "error": "method not allowed"}
            payload = parse_json_request(body)
            client_record = dashboard_store.update_client(
                client_id,
                name=json_field(payload, "name") if "name" in payload else None,
                notes=json_field(payload, "notes") if "notes" in payload else None,
                enabled=parse_bool_value(payload["enabled"]) if "enabled" in payload else None,
            )
            if client_record is None:
                return 404, {"ok": False, "error": "客户端不存在"}
            dashboard_audit(actor, "security", "client_update", target=f"client:{client_id}", summary=json.dumps(payload, ensure_ascii=False), result="success")
            return 200, {"ok": True, "client": client_record}

        if path == "/api/admin/power-settings":
            if auth["type"] != "admin":
                return 403, {"ok": False, "error": "admin required"}
            if method == "GET":
                return 200, {"ok": True, "settings": dashboard_store.power_settings()}
            if method == "PATCH":
                payload = parse_json_request(body)
                current = dashboard_store.power_settings()
                settings = dashboard_store.update_power_settings(
                    absolute_limit_w=float(payload.get("absolute_limit_w", current["absolute_limit_w"])),
                    relative_multiplier=float(payload.get("relative_multiplier", current["relative_multiplier"])),
                    relative_step_w=float(payload.get("relative_step_w", current["relative_step_w"])),
                )
                dashboard_audit(actor, "power", "power_settings", summary=json.dumps(settings, ensure_ascii=False), result="success")
                return 200, {"ok": True, "settings": settings}
            return 405, {"ok": False, "error": "method not allowed"}

        if path == "/api/admin/alerts":
            if method != "GET" or auth["type"] != "admin":
                return 405 if method != "GET" else 403, {"ok": False, "error": "method not allowed"}
            limit = int((query.get("limit") or ["100"])[0])
            active_only = (query.get("active") or ["0"])[0] in {"1", "true"}
            return 200, {"ok": True, "alerts": dashboard_store.power_alerts(limit, active_only=active_only)}

        if path.startswith("/api/admin/alerts/") and path.endswith("/ack"):
            if method != "POST" or auth["type"] != "admin":
                return 405 if method != "POST" else 403, {"ok": False, "error": "method not allowed"}
            alert_id = int(path.removeprefix("/api/admin/alerts/").removesuffix("/ack").strip("/"))
            alert = dashboard_store.acknowledge_power_alert(alert_id)
            if alert is None:
                return 404, {"ok": False, "error": "告警不存在"}
            dashboard_audit(actor, "power", "alert_ack", target=f"alert:{alert_id}", result="success")
            return 200, {"ok": True, "alert": alert}

        if path == "/api/admin/logs":
            if method != "GET" or auth["type"] != "admin":
                return 405 if method != "GET" else 403, {"ok": False, "error": "method not allowed"}
            return 200, {
                "ok": True,
                "records": dashboard_store.logs(
                    int((query.get("limit") or ["200"])[0]),
                    level=(query.get("level") or [""])[0],
                    module=(query.get("module") or [""])[0],
                    query=(query.get("q") or [""])[0],
                ),
            }

        if path == "/api/admin/firmware/devices":
            if auth["type"] != "admin":
                return 403, {"ok": False, "error": "admin required"}
            if method != "GET":
                return 405, {"ok": False, "error": "method not allowed"}
            return 200, {
                "ok": True,
                "devices": dashboard_store.ota_devices(
                    online_seconds=FRIDGE_ONLINE_TIMEOUT_SECONDS
                ),
            }

        if path == "/api/admin/firmware/deployments":
            if auth["type"] != "admin":
                return 403, {"ok": False, "error": "admin required"}
            if method == "GET":
                return 200, {
                    "ok": True,
                    "deployments": dashboard_store.ota_deployments(
                        device_id=(query.get("device_id") or [None])[0],
                        limit=int((query.get("limit") or ["50"])[0]),
                    ),
                }
            if method == "POST":
                payload = parse_json_request(body)
                device_id = json_field(payload, "device_id").strip()
                artifact_id = int(payload.get("artifact_id", 0))
                device = dashboard_store.ota_device(device_id)
                artifact = dashboard_store.firmware_artifact(artifact_id)
                if device is None:
                    return 404, {"ok": False, "error": "OTA设备尚未连接服务器"}
                if device_id != OTA_DEVICE_ID:
                    return 400, {"ok": False, "error": "设备ID不允许部署"}
                if (
                    artifact is None
                    or artifact.get("kind") != "ota"
                    or not bool(artifact.get("published"))
                    or not bool(artifact.get("image_valid"))
                ):
                    return 400, {"ok": False, "error": "请选择已发布且签名验证通过的OTA镜像"}
                if artifact.get("target_part") != device.get("part_number"):
                    return 400, {"ok": False, "error": "镜像目标芯片与设备不一致"}
                try:
                    if not version_is_newer(
                        str(artifact.get("image_version")),
                        str(device.get("app_version")),
                    ):
                        return 409, {"ok": False, "error": "目标版本必须高于设备当前版本"}
                    if parse_version(str(device.get("boot_version"))) < parse_version(
                        str(artifact.get("min_bootloader") or "0.0.0+0")
                    ):
                        return 409, {"ok": False, "error": "设备Bootloader版本过低"}
                except FirmwareImageError as exc:
                    return 400, {"ok": False, "error": str(exc)}
                try:
                    deployment = dashboard_store.create_ota_deployment(
                        secrets.token_hex(12),
                        device_id,
                        artifact_id,
                        str(device.get("app_version")),
                        str(artifact.get("image_version")),
                        int(artifact.get("size_bytes", 0)),
                        actor,
                    )
                    request_ota_device_reconnect(device_id)
                except ValueError as exc:
                    if str(exc) == "device_has_active_deployment":
                        return 409, {"ok": False, "error": "该设备已有未完成的升级任务"}
                    raise
                dashboard_audit(
                    actor,
                    "firmware",
                    "ota_deploy",
                    target=device_id,
                    summary=f"{deployment['from_version']} -> {deployment['target_version']}",
                    result="queued",
                )
                dashboard_event(
                    "firmware",
                    "warning",
                    "OTA升级已排队",
                    f"{device_id} -> {deployment['target_version']}",
                )
                return 201, {"ok": True, "deployment": deployment}
            return 405, {"ok": False, "error": "method not allowed"}

        deployment_cancel_match = re.fullmatch(
            r"/api/admin/firmware/deployments/([A-Za-z0-9_.-]+)/cancel",
            path,
        )
        if deployment_cancel_match is not None:
            if auth["type"] != "admin":
                return 403, {"ok": False, "error": "admin required"}
            if method != "POST":
                return 405, {"ok": False, "error": "method not allowed"}
            deployment_id = deployment_cancel_match.group(1)
            try:
                deployment = dashboard_store.cancel_ota_deployment(deployment_id)
            except ValueError:
                return 409, {"ok": False, "error": "升级已开始，不能从网页取消"}
            if deployment is None:
                return 404, {"ok": False, "error": "升级任务不存在"}
            dashboard_audit(
                actor,
                "firmware",
                "ota_cancel",
                target=deployment_id,
                result="cancelled",
            )
            return 200, {"ok": True, "deployment": deployment}

        if path == "/api/admin/firmware/releases":
            if auth["type"] != "admin":
                return 403, {"ok": False, "error": "admin required"}
            if method == "GET":
                return 200, {"ok": True, "releases": dashboard_store.firmware_releases(include_unpublished=True)}
            if method == "POST":
                payload = parse_json_request(body)
                component = json_field(payload, "component").strip().lower()
                channel = json_field(payload, "channel", "stable").strip().lower()
                if component not in FIRMWARE_COMPONENTS or channel not in FIRMWARE_CHANNELS:
                    return 400, {"ok": False, "error": "固件分类或发布通道无效"}
                release = dashboard_store.create_firmware_release(
                    component,
                    json_field(payload, "version"),
                    json_field(payload, "title"),
                    json_field(payload, "changelog"),
                    channel=channel,
                    published=parse_bool_value(payload.get("published", False)),
                    is_current=parse_bool_value(payload.get("is_current", False)),
                )
                dashboard_audit(actor, "firmware", "release_create", target=f"release:{release['id']}", summary=release.get("version", ""), result="success")
                return 201, {"ok": True, "release": release}
            return 405, {"ok": False, "error": "method not allowed"}

        if path.startswith("/api/admin/firmware/releases/"):
            if auth["type"] != "admin":
                return 403, {"ok": False, "error": "admin required"}
            release_id = int(path.removeprefix("/api/admin/firmware/releases/").strip("/"))
            if method == "PATCH":
                release = dashboard_store.update_firmware_release(release_id, parse_json_request(body))
                if release is None:
                    return 404, {"ok": False, "error": "版本不存在"}
                dashboard_audit(actor, "firmware", "release_update", target=f"release:{release_id}", result="success")
                return 200, {"ok": True, "release": release}
            if method == "DELETE":
                try:
                    artifacts = dashboard_store.delete_firmware_release(release_id)
                except ValueError:
                    return 409, {"ok": False, "error": "版本存在未完成的OTA任务，暂时不能删除"}
                if artifacts is None:
                    return 404, {"ok": False, "error": "版本不存在"}
                remove_unreferenced_firmware_files(artifacts)
                dashboard_audit(actor, "firmware", "release_delete", target=f"release:{release_id}", result="success")
                return 200, {"ok": True}
            return 405, {"ok": False, "error": "method not allowed"}

        if path.startswith("/api/admin/firmware/artifacts/"):
            if method != "DELETE" or auth["type"] != "admin":
                return 405 if method != "DELETE" else 403, {"ok": False, "error": "method not allowed"}
            artifact_id = int(path.removeprefix("/api/admin/firmware/artifacts/").strip("/"))
            try:
                artifact = dashboard_store.delete_firmware_artifact(artifact_id)
            except ValueError:
                return 409, {"ok": False, "error": "文件存在未完成的OTA任务，暂时不能删除"}
            if artifact is None:
                return 404, {"ok": False, "error": "文件不存在"}
            remove_unreferenced_firmware_files([artifact])
            dashboard_audit(actor, "firmware", "artifact_delete", target=f"artifact:{artifact_id}", result="success")
            return 200, {"ok": True}

        if path == "/api/ping" and method == "GET":
            return 200, {"ok": True, "message": "pong"}

        if path == "/api/fridge/status" and method == "GET":
            async with fridge_lock:
                refresh_control_commands_unlocked()
                return 200, {
                    "ok": True,
                    "status": fridge_snapshot_unlocked(),
                    "control": dict(fridge_control),
                    "control_pending": fridge_control_dirty,
                    "command": public_command(last_control_command),
                }

        if path == "/api/fridge/control" and method == "POST":
            payload = parse_json_request(body)
            async with fridge_lock:
                snapshot = fridge_snapshot_unlocked()
                if auth["type"] == "admin" and not snapshot["online"]:
                    return 409, {"ok": False, "error": "设备离线，控制指令未发送"}
                changed_fields: Dict[str, Any] = {}
                if "set_temp" in payload:
                    fridge_control["set_temp"] = clamp_int(int(payload["set_temp"]), -40, 80)
                    changed_fields["set_temp"] = fridge_control["set_temp"]
                if "fan" in payload:
                    fridge_control["fan"] = clamp_int(int(payload["fan"]), 0, 2)
                    changed_fields["fan"] = fridge_control["fan"]
                if "cool" in payload:
                    fridge_control["cool"] = parse_bool_value(payload["cool"])
                    changed_fields["cool"] = fridge_control["cool"]
                if "water_cool" in payload:
                    fridge_control["water_cool"] = parse_bool_value(payload["water_cool"])
                    changed_fields["water_cool"] = fridge_control["water_cool"]
                if "door" in payload:
                    fridge_control["door"] = parse_bool_value(payload["door"])
                    if fridge_control["door"]:
                        changed_fields["door"] = True
                if not changed_fields:
                    if auth["type"] == "app":
                        return 200, {
                            "ok": True,
                            "control": dict(fridge_control),
                            "control_pending": fridge_control_dirty,
                            "status": snapshot,
                            "command": public_command(last_control_command),
                        }
                    return 400, {"ok": False, "error": "没有可用的控制字段"}

                command = new_control_command(changed_fields, actor)
                has_persistent_control = any(key != "door" for key in changed_fields)
                if has_persistent_control:
                    if pending_control_command is not None and pending_control_command is not command:
                        set_command_result(pending_control_command, "superseded")
                    pending_control_command = command
                    fridge_control_dirty = True
                if changed_fields.get("door"):
                    if pending_door_command is not None and pending_door_command is not command:
                        set_command_result(pending_door_command, "superseded")
                    pending_door_command = command
                last_control_command = command
                dashboard_audit(
                    actor,
                    "device",
                    "fridge_control",
                    target=",".join(changed_fields.keys()),
                    summary=json.dumps(changed_fields, ensure_ascii=False, separators=(",", ":")),
                    result=str(command["status"]),
                    command_id=str(command["id"]),
                )
                return 200, {
                    "ok": True,
                    "control": dict(fridge_control),
                    "control_pending": fridge_control_dirty,
                    "status": snapshot,
                    "command_id": command["id"],
                    "command": public_command(command),
                }

        if path == "/api/vision/latest" and method == "GET":
            async with fridge_lock:
                return 200, {
                    "ok": True,
                    "vision": {
                        "slot": fridge_state.get("vision_slot"),
                        "code": fridge_state.get("vision_code"),
                        "name": fridge_state.get("vision_name", ""),
                        "updated_at": fridge_state.get("vision_updated_at", 0.0),
                    },
                }

        if path == "/api/medicines" and method == "GET":
            search = (query.get("q") or query.get("query") or [""])[0].strip().lower()
            async with medicine_lock:
                records = [medicine_record_for_api(record) for record in load_medicine_records()]
            if search:
                records = [
                    record for record in records
                    if search in record["name"].lower() or search in record["efficacy"].lower()
                ]
            return 200, {"ok": True, "records": records}

        if path == "/api/medicines" and method == "POST":
            payload = parse_json_request(body)
            quantity = parse_quantity(json_field(payload, "quantity", "1"))
            async with medicine_lock:
                records = load_medicine_records()
                empty_slots = [record["slot"] for record in records if not record.get("valid")]
                if not empty_slots:
                    return 400, {"ok": False, "error": "FULL"}
                slot = int(empty_slots[0])
                records[slot] = {
                    "valid": True,
                    "slot": slot,
                    "quantity": quantity,
                    "name": validate_medicine_field("name", json_field(payload, "name")),
                    "production_date": validate_medicine_field("production_date", json_field(payload, "production_date")),
                    "shelf_life": validate_medicine_field("shelf_life", json_field(payload, "shelf_life")),
                    "efficacy": validate_medicine_field("efficacy", json_field(payload, "efficacy")),
                }
                save_medicine_records(records)
                logger.info("HTTP medicine ADD slot=%s name=%s", slot, records[slot].get("name", ""))
                dashboard_audit(
                    actor,
                    "inventory",
                    "medicine_add",
                    target=f"slot:{slot}",
                    summary=f"{records[slot].get('name', '')} × {quantity}",
                    result="success",
                )
                return 201, {"ok": True, "record": medicine_record_for_api(records[slot])}

        if path.startswith("/api/medicines/"):
            suffix_parts = path.removeprefix("/api/medicines/").split("/")
            if len(suffix_parts) not in (1, 2):
                return 404, {"ok": False, "error": "not found"}
            slot = parse_slot(suffix_parts[0])

            if len(suffix_parts) == 2:
                if suffix_parts[1] != "quantity":
                    return 404, {"ok": False, "error": "not found"}
                if method != "PATCH":
                    return 405, {"ok": False, "error": "method not allowed"}
                payload = parse_json_request(body)
                operation = json_field(payload, "operation").strip().upper()
                protocol_operation = {"SET": "QTY", "RESTOCK": "RESTOCK", "TAKE": "TAKE"}.get(operation)
                if protocol_operation is None:
                    raise ValueError("operation must be set, restock or take")
                value = parse_quantity(
                    json_field(payload, "value"),
                    allow_zero=(protocol_operation == "QTY"),
                )
                async with medicine_lock:
                    records = load_medicine_records()
                    new_quantity = apply_medicine_quantity(records, slot, protocol_operation, value)
                    save_medicine_records(records)
                    record = medicine_record_for_api(records[slot])
                logger.info("HTTP medicine %s slot=%s quantity=%s", operation, slot, new_quantity)
                dashboard_audit(
                    actor,
                    "inventory",
                    f"quantity_{operation.lower()}",
                    target=f"slot:{slot}",
                    summary=f"value={value}, result={new_quantity}",
                    result="success",
                )
                return 200, {
                    "ok": True,
                    "operation": operation.lower(),
                    "quantity": new_quantity,
                    "record": record,
                }

            if method == "GET":
                async with medicine_lock:
                    record = medicine_record_for_api(load_medicine_records()[slot])
                if not record["valid"]:
                    return 404, {"ok": False, "error": "EMPTY"}
                return 200, {"ok": True, "record": record}

            if method == "PUT":
                payload = parse_json_request(body)
                async with medicine_lock:
                    records = load_medicine_records()
                    previous_quantity = (
                        int(records[slot].get("quantity", 1))
                        if records[slot].get("valid") else 1
                    )
                    quantity = (
                        parse_quantity(json_field(payload, "quantity"))
                        if "quantity" in payload else previous_quantity
                    )
                    records[slot] = {
                        "valid": True,
                        "slot": slot,
                        "quantity": quantity,
                        "name": validate_medicine_field("name", json_field(payload, "name")),
                        "production_date": validate_medicine_field("production_date", json_field(payload, "production_date")),
                        "shelf_life": validate_medicine_field("shelf_life", json_field(payload, "shelf_life")),
                        "efficacy": validate_medicine_field("efficacy", json_field(payload, "efficacy")),
                    }
                    save_medicine_records(records)
                    logger.info("HTTP medicine SET slot=%s name=%s", slot, records[slot].get("name", ""))
                    dashboard_audit(
                        actor,
                        "inventory",
                        "medicine_set",
                        target=f"slot:{slot}",
                        summary=f"{records[slot].get('name', '')} × {quantity}",
                        result="success",
                    )
                    return 200, {"ok": True, "record": medicine_record_for_api(records[slot])}

            if method == "DELETE":
                async with medicine_lock:
                    records = load_medicine_records()
                    old_name = records[slot].get("name", "")
                    records[slot] = make_blank_medicine_record(slot)
                    save_medicine_records(records)
                    logger.info("HTTP medicine DEL slot=%s old_name=%s", slot, old_name)
                dashboard_audit(
                    actor,
                    "inventory",
                    "medicine_delete",
                    target=f"slot:{slot}",
                    summary=str(old_name),
                    result="success",
                )
                return 200, {"ok": True, "slot": slot}

            return 405, {"ok": False, "error": "method not allowed"}

        if path == "/api/health/recommend" and method == "POST":
            started_at = time.perf_counter()
            symptom_for_history = ""
            try:
                payload = parse_json_request(body)
                symptom_for_history = json_field(payload, "symptom")
                symptom = validate_symptom_field(symptom_for_history)
                async with medicine_lock:
                    records = load_medicine_records()
                assessment = await ask_health_assessment(make_client(), symptom, records)
                diagnosis = assessment.diagnosis
                medicine = assessment.medicine
                latency_ms = (time.perf_counter() - started_at) * 1000.0
                dashboard_health(symptom, diagnosis, medicine, True, latency_ms)
                dashboard_event(
                    "ai",
                    "success",
                    f"手机AI初筛：{diagnosis}",
                    f"库存推荐：{medicine}",
                )
                dashboard_audit(
                    actor,
                    "ai",
                    "health_assess",
                    target="health_assistant",
                    summary=f"{diagnosis} → {medicine}",
                    result="success",
                )
                return 200, {
                    "ok": True,
                    "diagnosis": diagnosis,
                    "medicine": medicine,
                    # Keep the old field so an older App still displays the medicine.
                    "result": medicine,
                }
            except ValueError as exc:
                latency_ms = (time.perf_counter() - started_at) * 1000.0
                dashboard_health(
                    symptom_for_history,
                    "症状无效",
                    "无对应药品",
                    False,
                    latency_ms,
                )
                dashboard_event("ai", "warning", "手机健康助手请求无效", str(exc))
                dashboard_audit(actor, "ai", "health_assess", result="invalid")
                raise
            except Exception:
                latency_ms = (time.perf_counter() - started_at) * 1000.0
                dashboard_health(
                    symptom_for_history,
                    "诊断失败",
                    "无对应药品",
                    False,
                    latency_ms,
                )
                dashboard_event("ai", "danger", "手机健康助手请求失败", "DeepSeek调用异常")
                dashboard_audit(actor, "ai", "health_assess", result="failed")
                raise

        return 404, {"ok": False, "error": "not found"}
    except ValueError as exc:
        return 400, {"ok": False, "error": str(exc)}
    except json.JSONDecodeError as exc:
        return 400, {"ok": False, "error": f"invalid JSON: {exc}"}
    except Exception as exc:
        logger.exception("HTTP API failed: %s %s", method, path)
        return 500, {"ok": False, "error": str(exc)}


def http_metric_operation(method: str, target: str) -> tuple[str, str]:
    path = unquote(urlparse(target).path)
    if path.startswith("/api/public/dashboard/") or path.startswith("/api/admin/") or path in STATIC_DASHBOARD_FILES:
        return "dashboard", "dashboard_view"
    if path == "/api/health/recommend":
        return "http", "medicine_recommend"
    clean = path.strip("/").replace("/", "_") or "root"
    return "http", f"{method.lower()}_{clean}"[:48]


async def _send_admin_stream(headers: Dict[str, str], writer: asyncio.StreamWriter) -> bool:
    """Send authenticated one-second device snapshots using Server-Sent Events."""
    auth, auth_error = authenticate_http_request("GET", headers)
    if auth is None or auth.get("type") != "admin":
        await send_http_json(writer, 401, {"ok": False, "error": auth_error or "unauthorized"})
        return False

    response_headers = [
        "HTTP/1.1 200 OK",
        "Content-Type: text/event-stream; charset=utf-8",
        "Cache-Control: no-store",
        "Connection: keep-alive",
        "X-Accel-Buffering: no",
        "X-Content-Type-Options: nosniff",
        "X-Frame-Options: DENY",
        "Referrer-Policy: no-referrer",
        "",
        "",
    ]
    writer.write("\r\n".join(response_headers).encode("ascii"))
    await writer.drain()

    last_payload = ""
    for sequence in range(300):
        if admin_session_from_headers(headers, touch=(sequence % 30 == 0)) is None:
            writer.write(b"event: session\ndata: {\"authenticated\":false}\n\n")
            await writer.drain()
            return True
        async with fridge_lock:
            refresh_control_commands_unlocked()
            payload = {
                "generated_at": time.time(),
                "status": fridge_snapshot_unlocked(),
                "control": dict(fridge_control),
                "control_pending": fridge_control_dirty,
                "command": public_command(last_control_command),
                "clients": dashboard_store.client_summary() if dashboard_store is not None else {},
                "power_settings": dashboard_store.power_settings() if dashboard_store is not None else {},
                "power_alerts": dashboard_store.power_alerts(5, active_only=True) if dashboard_store is not None else [],
                "recognitions": dashboard_store.recent_vision_recognitions(5) if dashboard_store is not None else [],
            }
        encoded = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
        if encoded != last_payload or sequence % 15 == 0:
            writer.write(f"event: snapshot\ndata: {encoded}\n\n".encode("utf-8"))
            await writer.drain()
            last_payload = encoded
        await asyncio.sleep(1.0)
    return True


async def send_admin_stream(headers: Dict[str, str], writer: asyncio.StreamWriter) -> bool:
    try:
        return await _send_admin_stream(headers, writer)
    except (ConnectionError, OSError):
        # EventSource reconnects normally; a closed browser tab is not a bad HTTP request.
        return True


async def handle_http_client(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
    peer = writer.get_extra_info("peername")
    started_at = time.perf_counter()
    metric_channel = "http"
    metric_operation = "bad_request"
    metric_success = False
    try:
        raw_headers = await reader.readuntil(b"\r\n\r\n")
        method, target, headers = decode_http_headers(raw_headers[:-4])
        metric_channel, metric_operation = http_metric_operation(method, target)
        if await send_firmware_download(method, target, writer):
            metric_success = method == "GET"
            return
        if await send_dashboard_asset(method, target, writer):
            metric_success = method == "GET"
            return
        path = unquote(urlparse(target).path)
        if path == "/api/admin/stream":
            if method != "GET":
                await send_http_json(writer, 405, {"ok": False, "error": "method not allowed"})
                return
            metric_success = await send_admin_stream(headers, writer)
            return
        content_length = int(headers.get("content-length", "0") or "0")
        if path == "/api/admin/firmware/artifacts" and method == "POST":
            status, payload = await receive_firmware_upload(reader, target, headers, content_length)
            await send_http_json(writer, status, payload)
            metric_success = 200 <= status < 400
            return
        if content_length > MAX_JSON_BODY_BYTES:
            await send_http_json(writer, 413, {"ok": False, "error": "request body too large"})
            return

        body = await reader.readexactly(content_length) if content_length > 0 else b""
        response_headers: Dict[str, str] = {}
        peer_ip = str(peer[0]) if isinstance(peer, tuple) and peer else "unknown"
        status, payload = await handle_http_api(
            method,
            target,
            headers,
            body,
            peer_ip=peer_ip,
            response_headers=response_headers,
        )
        await send_http_json(writer, status, payload, extra_headers=response_headers)
        metric_success = 200 <= status < 400
    except (ConnectionError, OSError):
        pass
    except Exception as exc:
        logger.warning("bad HTTP request from %s: %s", peer, exc)
        await send_http_json(writer, 400, {"ok": False, "error": "bad request"})
    finally:
        dashboard_metric(metric_channel, metric_operation, metric_success, started_at)
        writer.close()
        with contextlib.suppress(ConnectionError, OSError):
            await writer.wait_closed()


def w800_metric_operation(user_text: str) -> str:
    upper = user_text.upper()
    if user_text.lower() in {"/ping", "ping"}:
        return "ping"
    if upper.startswith("AUD1 "):
        return "speech"
    if upper.startswith("FW1 "):
        return "firmware_ota"
    if upper.startswith("FRIDGE STATUS "):
        return "fridge_status"
    if upper.startswith("HA ASSESS "):
        return "health_assess"
    if upper.startswith("MED RECOMMEND "):
        return "medicine_recommend"
    if upper.startswith("MED "):
        return "medicine_command"
    return "chat"


class FirmwareProtocolError(RuntimeError):
    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = re.sub(r"[^A-Za-z0-9_.-]", "_", code)[:48] or "protocol_error"


def ota_token_valid(token: str) -> bool:
    return bool(OTA_DEVICE_TOKEN) and hmac.compare_digest(str(token), OTA_DEVICE_TOKEN)


def request_ota_device_reconnect(device_id: str) -> bool:
    """Wake a busy device by closing its FW1-registered TCP connection."""
    writer = OTA_DEVICE_WRITERS.get(str(device_id))
    if writer is None or writer.is_closing():
        return False
    writer.close()
    return True


async def send_fw1_line(writer: asyncio.StreamWriter, line: str) -> None:
    writer.write(line.encode("ascii") + b"\n")
    await writer.drain()


def ota_artifact_path(artifact: Dict[str, Any]) -> Path:
    root = FIRMWARE_ROOT.resolve()
    candidate = (root / str(artifact.get("stored_name") or "")).resolve()
    if candidate.parent != root or not candidate.is_file():
        raise FirmwareProtocolError("image_missing", "OTA image is missing")
    return candidate


def ota_require_token(token: str) -> None:
    if not ota_token_valid(token):
        raise FirmwareProtocolError("unauthorized", "Invalid OTA device token")


async def handle_fw1_check(parts: List[str]) -> str:
    if dashboard_store is None:
        raise FirmwareProtocolError("server_starting", "Dashboard store is unavailable")
    if len(parts) != 8:
        raise FirmwareProtocolError("bad_check", "Invalid FW1 CHECK fields")
    _, _, device_id, part_number, app_version, boot_version, bank_text, token = parts
    ota_require_token(token)
    if device_id != OTA_DEVICE_ID:
        raise FirmwareProtocolError("unknown_device", "Unknown OTA device")
    if part_number != OTA_TARGET_PART:
        raise FirmwareProtocolError("wrong_part", "Device part number is not allowed")
    try:
        app_version = normalize_version(app_version)
        boot_version = normalize_version(boot_version)
        bank = int(bank_text, 10)
    except (FirmwareImageError, ValueError) as exc:
        raise FirmwareProtocolError("bad_device_version", str(exc)) from exc
    if bank not in (0, 1):
        raise FirmwareProtocolError("bad_bank", "Active bank must be 0 or 1")

    device = dashboard_store.upsert_ota_device(
        device_id,
        part_number,
        app_version,
        boot_version,
        bank,
    )
    deployment = dashboard_store.active_ota_deployment(device_id)
    if deployment is None:
        return "FW1 NONE"

    if deployment["status"] in {"rebooting", "trial"}:
        if app_version == deployment["target_version"]:
            dashboard_store.update_ota_deployment(deployment["id"], status="trial")
            dashboard_store.upsert_ota_device(
                device_id,
                part_number,
                app_version,
                boot_version,
                bank,
                ota_state="trial",
                current_deployment_id=deployment["id"],
                received_bytes=int(deployment["bytes_total"]),
                total_bytes=int(deployment["bytes_total"]),
            )
        return "FW1 NONE"

    artifact = dashboard_store.firmware_artifact(int(deployment["artifact_id"]))
    if artifact is None or not bool(artifact.get("image_valid")):
        dashboard_store.update_ota_deployment(
            deployment["id"],
            status="failed",
            error_code="image_not_deployable",
        )
        raise FirmwareProtocolError("image_not_deployable", "OTA artifact is not valid")
    if artifact.get("target_part") != part_number:
        dashboard_store.update_ota_deployment(
            deployment["id"],
            status="failed",
            error_code="target_mismatch",
        )
        raise FirmwareProtocolError("target_mismatch", "OTA artifact target does not match")
    try:
        if not version_is_newer(str(deployment["target_version"]), app_version):
            raise FirmwareProtocolError("version_not_newer", "Target version is not newer")
        if parse_version(boot_version) < parse_version(str(artifact.get("min_bootloader") or "0.0.0+0")):
            raise FirmwareProtocolError("bootloader_too_old", "Bootloader version is too old")
    except FirmwareImageError as exc:
        raise FirmwareProtocolError("bad_version", str(exc)) from exc
    try:
        image_path = ota_artifact_path(artifact)
    except FirmwareProtocolError as exc:
        dashboard_store.update_ota_deployment(
            deployment["id"],
            status="failed",
            error_code=exc.code,
        )
        raise
    if image_path.stat().st_size != int(deployment["bytes_total"]):
        dashboard_store.update_ota_deployment(
            deployment["id"],
            status="failed",
            error_code="image_size_changed",
        )
        raise FirmwareProtocolError("image_size_changed", "OTA image size changed")

    dashboard_store.update_ota_deployment(deployment["id"], status="offered")
    dashboard_store.upsert_ota_device(
        device_id,
        part_number,
        app_version,
        boot_version,
        bank,
        ota_state="offered",
        current_deployment_id=deployment["id"],
        received_bytes=int(deployment["bytes_sent"]),
        total_bytes=int(deployment["bytes_total"]),
    )
    return (
        f"FW1 OFFER {deployment['id']} {deployment['target_version']} "
        f"{deployment['bytes_total']} {artifact['sha256']} {OTA_CHUNK_BYTES}"
    )


async def handle_fw1_start(parts: List[str], writer: asyncio.StreamWriter) -> None:
    if dashboard_store is None:
        raise FirmwareProtocolError("server_starting", "Dashboard store is unavailable")
    if len(parts) != 5:
        raise FirmwareProtocolError("bad_start", "Invalid FW1 START fields")
    _, _, deployment_id, offset_text, token = parts
    ota_require_token(token)
    deployment = dashboard_store.ota_deployment(deployment_id)
    if deployment is None or deployment["device_id"] != OTA_DEVICE_ID:
        raise FirmwareProtocolError("unknown_deployment", "Unknown OTA deployment")
    if deployment["status"] not in {"queued", "offered", "transferring"}:
        raise FirmwareProtocolError("deployment_not_active", "OTA deployment is not transferable")
    try:
        offset = int(offset_text, 10)
    except ValueError as exc:
        raise FirmwareProtocolError("bad_offset", "Invalid OTA resume offset") from exc
    total = int(deployment["bytes_total"])
    if offset < 0 or offset > total or (offset != total and offset % OTA_CHUNK_BYTES):
        raise FirmwareProtocolError("bad_offset", "OTA resume offset is not aligned")

    artifact = dashboard_store.firmware_artifact(int(deployment["artifact_id"]))
    if artifact is None or not bool(artifact.get("image_valid")):
        raise FirmwareProtocolError("image_not_deployable", "OTA artifact is not valid")
    image_path = ota_artifact_path(artifact)
    dashboard_store.update_ota_deployment(
        deployment_id,
        status="transferring",
        bytes_sent=offset,
    )
    dashboard_store.upsert_ota_device(
        OTA_DEVICE_ID,
        str(artifact["target_part"]),
        str(deployment["from_version"]),
        str((dashboard_store.ota_device(OTA_DEVICE_ID) or {}).get("boot_version", "0.0.0+0")),
        int((dashboard_store.ota_device(OTA_DEVICE_ID) or {}).get("active_bank", 0)),
        ota_state="transferring",
        current_deployment_id=deployment_id,
        received_bytes=offset,
        total_bytes=total,
    )

    sequence = offset // OTA_CHUNK_BYTES
    sent = offset
    with image_path.open("rb") as stream:
        stream.seek(offset)
        while sent < total:
            chunk = stream.read(min(OTA_CHUNK_BYTES, total - sent))
            if not chunk:
                raise FirmwareProtocolError("image_truncated", "OTA image ended unexpectedly")
            checksum = zlib.crc32(chunk) & 0xFFFFFFFF
            await send_fw1_line(
                writer,
                f"FW1 DATA {deployment_id} {sequence} {sent} {len(chunk)} {checksum:08X}",
            )
            writer.write(chunk)
            await writer.drain()
            sent += len(chunk)
            sequence += 1
            if sent == total or sent % (32 * 1024) == 0:
                dashboard_store.update_ota_deployment(
                    deployment_id,
                    status="transferring",
                    bytes_sent=sent,
                )
    await send_fw1_line(
        writer,
        f"FW1 END {deployment_id} {total} {artifact['sha256']}",
    )
    dashboard_store.update_ota_deployment(
        deployment_id,
        status="rebooting",
        bytes_sent=total,
    )


async def handle_fw1_progress(parts: List[str]) -> str:
    if dashboard_store is None:
        raise FirmwareProtocolError("server_starting", "Dashboard store is unavailable")
    if len(parts) != 6:
        raise FirmwareProtocolError("bad_progress", "Invalid FW1 PROGRESS fields")
    _, _, deployment_id, received_text, state, token = parts
    ota_require_token(token)
    deployment = dashboard_store.ota_deployment(deployment_id)
    if deployment is None or deployment["device_id"] != OTA_DEVICE_ID:
        raise FirmwareProtocolError("unknown_deployment", "Unknown OTA deployment")
    try:
        received = int(received_text, 10)
    except ValueError as exc:
        raise FirmwareProtocolError("bad_progress", "Invalid OTA byte count") from exc
    state_map = {
        "erasing": "transferring",
        "receiving": "transferring",
        "writing": "transferring",
        "verifying": "transferring",
        "waiting_reboot": "rebooting",
        "trial": "trial",
    }
    if state not in state_map:
        raise FirmwareProtocolError("bad_progress_state", "Invalid OTA progress state")
    status = state_map[state]
    dashboard_store.update_ota_deployment(
        deployment_id,
        status=status,
        bytes_sent=received,
    )
    device = dashboard_store.ota_device(OTA_DEVICE_ID) or {}
    dashboard_store.upsert_ota_device(
        OTA_DEVICE_ID,
        str(device.get("part_number", OTA_TARGET_PART)),
        str(device.get("app_version", deployment["from_version"])),
        str(device.get("boot_version", "0.0.0+0")),
        int(device.get("active_bank", 0)),
        ota_state=state,
        current_deployment_id=deployment_id,
        received_bytes=received,
        total_bytes=int(deployment["bytes_total"]),
    )
    return f"FW1 OK {deployment_id}"


async def handle_fw1_result(parts: List[str]) -> str:
    if dashboard_store is None:
        raise FirmwareProtocolError("server_starting", "Dashboard store is unavailable")
    if len(parts) != 8:
        raise FirmwareProtocolError("bad_result", "Invalid FW1 RESULT fields")
    _, _, deployment_id, version, bank_text, result, error_code, token = parts
    ota_require_token(token)
    deployment = dashboard_store.ota_deployment(deployment_id)
    if deployment is None or deployment["device_id"] != OTA_DEVICE_ID:
        raise FirmwareProtocolError("unknown_deployment", "Unknown OTA deployment")
    try:
        version = normalize_version(version)
        bank = int(bank_text, 10)
    except (FirmwareImageError, ValueError) as exc:
        raise FirmwareProtocolError("bad_result", str(exc)) from exc
    if bank not in (0, 1):
        raise FirmwareProtocolError("bad_bank", "Active bank must be 0 or 1")
    status_map = {
        "success": "succeeded",
        "failed": "failed",
        "rolled_back": "rolled_back",
    }
    if result not in status_map:
        raise FirmwareProtocolError("bad_result_state", "Invalid OTA result state")
    if result == "success" and version != deployment["target_version"]:
        raise FirmwareProtocolError("version_mismatch", "Confirmed version does not match deployment")

    status = status_map[result]
    dashboard_store.update_ota_deployment(
        deployment_id,
        status=status,
        bytes_sent=int(deployment["bytes_total"]),
        error_code="" if error_code == "-" else error_code,
    )
    device = dashboard_store.ota_device(OTA_DEVICE_ID) or {}
    dashboard_store.upsert_ota_device(
        OTA_DEVICE_ID,
        str(device.get("part_number", OTA_TARGET_PART)),
        version,
        str(device.get("boot_version", "0.0.0+0")),
        bank,
        ota_state="idle",
        current_deployment_id="",
        received_bytes=0,
        total_bytes=0,
        last_result=result,
        last_error="" if error_code == "-" else error_code,
    )
    dashboard_event(
        "firmware",
        "success" if result == "success" else "warning",
        f"OTA {result}",
        f"{OTA_DEVICE_ID} {version}",
    )
    return f"FW1 OK {deployment_id}"


async def handle_fw1_command(
    reader: asyncio.StreamReader,
    writer: asyncio.StreamWriter,
    user_text: str,
) -> None:
    del reader
    parts = user_text.split()
    command = parts[1].upper() if len(parts) >= 2 else ""
    try:
        if command == "CHECK":
            reply = await handle_fw1_check(parts)
            if len(parts) >= 3:
                OTA_DEVICE_WRITERS[parts[2]] = writer
            writer.write(f"{reply}\r\n[END]\r\n".encode("ascii"))
            await writer.drain()
            return
        if command == "START":
            await handle_fw1_start(parts, writer)
            return
        if command == "PROGRESS":
            await handle_fw1_progress(parts)
            return
        if command == "RESULT":
            await handle_fw1_result(parts)
            return
        raise FirmwareProtocolError("unknown_command", "Unsupported FW1 command")
    except FirmwareProtocolError as exc:
        logger.warning("FW1 request rejected: %s", exc.code)
        if command == "CHECK":
            writer.write(f"FW1 ERROR {exc.code}\r\n[END]\r\n".encode("ascii"))
            await writer.drain()
        else:
            await send_fw1_line(writer, f"FW1 ERROR {exc.code}")


class AudioProtocolError(RuntimeError):
    """A malformed AUD1 request. Fatal errors require a fresh TCP connection."""

    def __init__(self, code: str, message: str, *, fatal: bool = False) -> None:
        super().__init__(message)
        self.code = code
        self.fatal = fatal


async def send_audio_line(writer: asyncio.StreamWriter, line: str) -> None:
    writer.write(line.encode("ascii") + b"\n")
    await writer.drain()


async def send_audio_error(
    writer: asyncio.StreamWriter,
    session_id: str,
    code: str,
    message: str,
) -> None:
    safe_code = re.sub(r"[^A-Za-z0-9_.-]", "_", code)[:48] or "speech_error"
    body = message.encode("utf-8", errors="replace")[:512]
    await send_audio_line(
        writer,
        f"AUD1 ERROR {session_id} {safe_code} {len(body)}",
    )
    writer.write(body)
    await writer.drain()


async def send_audio_health(
    writer: asyncio.StreamWriter,
    session_id: str,
    diagnosis: str,
    medicine: str,
) -> None:
    """Send one length-delimited UTF-8 diagnosis/medicine result."""

    if any((ord(ch) < 0x20 or ord(ch) == 0x7F) for ch in diagnosis + medicine):
        raise SpeechPipelineError(
            "health_payload_invalid",
            "Health result contains a control character",
        )

    diagnosis_bytes = diagnosis.encode("utf-8")
    medicine_bytes = medicine.encode("utf-8")
    if not diagnosis_bytes or len(diagnosis_bytes) > AUDIO_HEALTH_DIAGNOSIS_MAX_BYTES:
        raise SpeechPipelineError(
            "health_payload_invalid",
            "Diagnosis result is empty or too long",
        )
    if not medicine_bytes or len(medicine_bytes) > AUDIO_HEALTH_MEDICINE_MAX_BYTES:
        raise SpeechPipelineError(
            "health_payload_invalid",
            "Medicine result is empty or too long",
        )

    await send_audio_line(
        writer,
        f"AUD1 HEALTH {session_id} {len(diagnosis_bytes)} {len(medicine_bytes)}",
    )
    writer.write(diagnosis_bytes)
    writer.write(medicine_bytes)
    await writer.drain()


def parse_audio_start(line: str) -> tuple[str, str]:
    parts = line.split(" ")
    if len(parts) != 7 or parts[0:2] != ["AUD1", "START"]:
        raise AudioProtocolError("bad_start", "Invalid AUD1 START header")

    session_id, sample_rate, channels, audio_format, token = parts[2:]
    if re.fullmatch(r"[A-Za-z0-9_-]{1,32}", session_id) is None:
        raise AudioProtocolError("bad_session", "Invalid audio session ID")
    if sample_rate != str(AUDIO_SAMPLE_RATE) or channels != "1" or audio_format != "S16LE":
        raise AudioProtocolError(
            "unsupported_format",
            "Only 16000 Hz mono S16LE is supported",
        )
    return session_id, token


def parse_audio_say(line: str) -> tuple[str, int, str]:
    parts = line.split(" ")
    if len(parts) != 5 or parts[0:2] != ["AUD1", "SAY"]:
        raise AudioProtocolError("bad_say", "Invalid AUD1 SAY header", fatal=True)

    session_id, text_length_text, token = parts[2:]
    if re.fullmatch(r"[A-Za-z0-9_-]{1,32}", session_id) is None:
        raise AudioProtocolError("bad_session", "Invalid audio session ID", fatal=True)
    text_length = parse_positive_int(
        text_length_text,
        "text_bytes",
        AUDIO_SAY_MAX_TEXT_BYTES,
    )
    if text_length == 0:
        raise AudioProtocolError("empty_text", "AUD1 SAY text cannot be empty", fatal=True)
    return session_id, text_length, token


def parse_positive_int(value: str, field: str, maximum: int) -> int:
    try:
        parsed = int(value, 10)
    except ValueError as exc:
        raise AudioProtocolError("bad_number", f"Invalid {field}", fatal=True) from exc
    if parsed < 0 or parsed > maximum:
        raise AudioProtocolError("bad_number", f"{field} is out of range", fatal=True)
    return parsed


async def send_audio_pcm_frame(
    writer: asyncio.StreamWriter,
    session_id: str,
    sequence: int,
    pcm: bytes,
) -> None:
    checksum = zlib.crc32(pcm) & 0xFFFFFFFF
    await send_audio_line(
        writer,
        f"AUD1 PCM {session_id} {sequence} {len(pcm)} {checksum:08X}",
    )
    writer.write(pcm)
    await writer.drain()


async def stream_tts_pcm(
    writer: asyncio.StreamWriter,
    session_id: str,
    text: str,
) -> tuple[int, int]:
    """Synthesize *text* unchanged and stream framed PCM16 to the device."""

    pcm_buffer = bytearray()
    pcm_sequence = 0
    total_pcm_bytes = 0
    async for chunk in speech_pipeline.synthesize(text):
        if total_pcm_bytes + len(pcm_buffer) + len(chunk) > AUDIO_MAX_TTS_BYTES:
            raise SpeechPipelineError(
                "tts_too_long",
                "TTS output exceeds 60 seconds",
            )
        pcm_buffer.extend(chunk)
        while len(pcm_buffer) >= AUDIO_TTS_FRAME_BYTES:
            frame = bytes(pcm_buffer[:AUDIO_TTS_FRAME_BYTES])
            del pcm_buffer[:AUDIO_TTS_FRAME_BYTES]
            await send_audio_pcm_frame(writer, session_id, pcm_sequence, frame)
            pcm_sequence += 1
            total_pcm_bytes += len(frame)

    if len(pcm_buffer) % 2 != 0:
        raise SpeechPipelineError(
            "invalid_tts_pcm",
            "TTS returned an odd number of PCM bytes",
        )
    if pcm_buffer:
        frame = bytes(pcm_buffer)
        await send_audio_pcm_frame(writer, session_id, pcm_sequence, frame)
        pcm_sequence += 1
        total_pcm_bytes += len(frame)

    return pcm_sequence, total_pcm_bytes


async def handle_audio_say(
    reader: asyncio.StreamReader,
    writer: asyncio.StreamWriter,
    start_line: str,
) -> None:
    """Receive one UTF-8 label and return only its CosyVoice PCM stream."""

    session_id = "0"
    started_at = time.perf_counter()
    try:
        preliminary_parts = start_line.split(" ")
        if (
            len(preliminary_parts) >= 3
            and re.fullmatch(r"[A-Za-z0-9_-]{1,32}", preliminary_parts[2]) is not None
        ):
            session_id = preliminary_parts[2]
        session_id, text_length, supplied_token = parse_audio_say(start_line)
        if not AUDIO_DEVICE_TOKEN:
            raise AudioProtocolError(
                "audio_not_configured",
                "FRIDGE_AUDIO_TOKEN is not configured on the server",
                fatal=True,
            )
        if not hmac.compare_digest(supplied_token, AUDIO_DEVICE_TOKEN):
            raise AudioProtocolError(
                "unauthorized",
                "Invalid audio device token",
                fatal=True,
            )

        try:
            text_bytes = await asyncio.wait_for(
                reader.readexactly(text_length),
                timeout=AUDIO_FRAME_TIMEOUT_SECONDS,
            )
        except (TimeoutError, asyncio.IncompleteReadError) as exc:
            raise AudioProtocolError(
                "short_text",
                "AUD1 SAY text payload is incomplete",
                fatal=True,
            ) from exc

        try:
            text = text_bytes.decode("utf-8", errors="strict")
        except UnicodeDecodeError as exc:
            raise AudioProtocolError(
                "bad_utf8",
                "AUD1 SAY text is not valid UTF-8",
                fatal=True,
            ) from exc
        if any(ord(character) < 0x20 or ord(character) == 0x7F for character in text):
            raise AudioProtocolError(
                "control_character",
                "AUD1 SAY text contains a control character",
                fatal=True,
            )

        await send_audio_line(writer, f"AUD1 READY {session_id}")
        pcm_sequence, total_pcm_bytes = await stream_tts_pcm(
            writer,
            session_id,
            text,
        )
        await send_audio_line(
            writer,
            f"AUD1 DONE {session_id} {pcm_sequence} {total_pcm_bytes}",
        )
        dashboard_metric("w800", "speech", True, started_at)
        dashboard_event(
            "speech",
            "success",
            "药品名称语音播报完成",
            f"session={session_id} text_bytes={text_length} tts_bytes={total_pcm_bytes}",
        )
    except AudioProtocolError as exc:
        with contextlib.suppress(ConnectionError, OSError):
            await send_audio_error(writer, session_id, exc.code, str(exc))
        dashboard_metric("w800", "speech", False, started_at)
        dashboard_event("speech", "warning", "药名播报协议失败", f"{exc.code}: {exc}")
        if exc.fatal:
            raise
    except SpeechPipelineError as exc:
        with contextlib.suppress(ConnectionError, OSError):
            await send_audio_error(writer, session_id, exc.code, str(exc))
        dashboard_metric("w800", "speech", False, started_at)
        dashboard_event("speech", "danger", "药名语音合成失败", f"{exc.code}: {exc}")
    except Exception as exc:
        logger.exception("unexpected medicine speech failure")
        with contextlib.suppress(ConnectionError, OSError):
            await send_audio_error(writer, session_id, "internal_error", "Speech service failed")
        dashboard_metric("w800", "speech", False, started_at)
        dashboard_event("speech", "danger", "药名语音服务异常", type(exc).__name__)


async def handle_audio_session(
    reader: asyncio.StreamReader,
    writer: asyncio.StreamWriter,
    start_line: str,
    client: AsyncOpenAI,
) -> None:
    """Receive PCM, return raw ASR text, then speak one health assessment."""

    session_id = "0"
    asr_session = None
    asr_finished = False
    started_at = time.perf_counter()
    try:
        session_id, supplied_token = parse_audio_start(start_line)
        if not AUDIO_DEVICE_TOKEN:
            raise AudioProtocolError(
                "audio_not_configured",
                "FRIDGE_AUDIO_TOKEN is not configured on the server",
            )
        if not hmac.compare_digest(supplied_token, AUDIO_DEVICE_TOKEN):
            raise AudioProtocolError("unauthorized", "Invalid audio device token")

        asr_session = speech_pipeline.create_asr_session()
        asr_session.start()
        await send_audio_line(writer, f"AUD1 READY {session_id}")

        expected_sequence = 0
        total_samples = 0
        while True:
            try:
                raw_line = await asyncio.wait_for(
                    reader.readline(),
                    timeout=AUDIO_FRAME_TIMEOUT_SECONDS,
                )
            except TimeoutError as exc:
                raise AudioProtocolError(
                    "frame_timeout",
                    "Timed out waiting for an audio frame",
                    fatal=True,
                ) from exc
            if not raw_line:
                raise AudioProtocolError(
                    "connection_closed",
                    "Device disconnected during audio upload",
                    fatal=True,
                )
            try:
                line = raw_line.decode("ascii").rstrip("\r\n")
            except UnicodeDecodeError as exc:
                raise AudioProtocolError(
                    "bad_header",
                    "Audio frame header is not ASCII",
                    fatal=True,
                ) from exc
            parts = line.split(" ")

            if len(parts) == 6 and parts[0:2] == ["AUD1", "DATA"]:
                if parts[2] != session_id:
                    raise AudioProtocolError(
                        "session_mismatch",
                        "Audio frame session ID mismatch",
                        fatal=True,
                    )
                sequence = parse_positive_int(parts[3], "sequence", AUDIO_MAX_FRAMES)
                sample_count = parse_positive_int(
                    parts[4],
                    "sample_count",
                    AUDIO_FRAME_SAMPLES,
                )
                if sample_count == 0:
                    raise AudioProtocolError(
                        "empty_frame",
                        "Audio DATA frame cannot be empty",
                        fatal=True,
                    )
                if sequence != expected_sequence:
                    raise AudioProtocolError(
                        "sequence_mismatch",
                        f"Expected frame {expected_sequence}, received {sequence}",
                        fatal=True,
                    )
                if expected_sequence >= AUDIO_MAX_FRAMES:
                    raise AudioProtocolError(
                        "audio_too_long",
                        "Audio session exceeds 30 seconds",
                        fatal=True,
                    )
                try:
                    expected_crc = int(parts[5], 16)
                except ValueError as exc:
                    raise AudioProtocolError(
                        "bad_crc",
                        "Invalid audio frame CRC",
                        fatal=True,
                    ) from exc

                try:
                    pcm = await asyncio.wait_for(
                        reader.readexactly(sample_count * 2),
                        timeout=AUDIO_FRAME_TIMEOUT_SECONDS,
                    )
                except (TimeoutError, asyncio.IncompleteReadError) as exc:
                    raise AudioProtocolError(
                        "short_frame",
                        "Audio frame payload is incomplete",
                        fatal=True,
                    ) from exc
                actual_crc = zlib.crc32(pcm) & 0xFFFFFFFF
                if actual_crc != expected_crc:
                    raise AudioProtocolError(
                        "crc_mismatch",
                        f"Audio frame {sequence} CRC mismatch",
                        fatal=True,
                    )

                await asr_session.push(pcm)
                expected_sequence += 1
                total_samples += sample_count
                continue

            if len(parts) == 5 and parts[0:2] == ["AUD1", "END"]:
                if parts[2] != session_id:
                    raise AudioProtocolError(
                        "session_mismatch",
                        "Audio END session ID mismatch",
                        fatal=True,
                    )
                frame_count = parse_positive_int(
                    parts[3],
                    "frame_count",
                    AUDIO_MAX_FRAMES,
                )
                declared_samples = parse_positive_int(
                    parts[4],
                    "total_samples",
                    AUDIO_MAX_FRAMES * AUDIO_FRAME_SAMPLES,
                )
                if frame_count != expected_sequence or declared_samples != total_samples:
                    raise AudioProtocolError(
                        "length_mismatch",
                        "Audio END counters do not match received data",
                        fatal=True,
                    )
                break

            raise AudioProtocolError(
                "bad_audio_command",
                "Expected AUD1 DATA or AUD1 END",
                fatal=True,
            )

        transcript = await asr_session.finish()
        asr_finished = True
        transcript_bytes = transcript.encode("utf-8")
        await send_audio_line(
            writer,
            f"AUD1 TEXT {session_id} {len(transcript_bytes)}",
        )
        writer.write(transcript_bytes)
        await writer.drain()

        pcm_sequence = 0
        total_pcm_bytes = 0
        if transcript != "":
            try:
                async with medicine_lock:
                    records = load_medicine_records()
                assessment = await ask_health_assessment(
                    client,
                    transcript,
                    records,
                )
                diagnosis = assessment.diagnosis
                medicine = assessment.medicine
                spoken_reply = format_voice_health_reply(assessment)
                dashboard_health(
                    transcript,
                    diagnosis,
                    medicine,
                    True,
                    (time.perf_counter() - started_at) * 1000.0,
                )
                dashboard_event(
                    "ai",
                    "success",
                    "语音健康初筛完成",
                    f"session={session_id} medicine={medicine}",
                )
            except Exception as exc:
                logger.exception("voice health assessment failed")
                raise SpeechPipelineError(
                    "health_assessment_failed",
                    "Health assessment failed",
                ) from exc

            await send_audio_health(
                writer,
                session_id,
                diagnosis,
                medicine,
            )

            pcm_sequence, total_pcm_bytes = await stream_tts_pcm(
                writer,
                session_id,
                spoken_reply,
            )

        await send_audio_line(
            writer,
            f"AUD1 DONE {session_id} {pcm_sequence} {total_pcm_bytes}",
        )
        dashboard_metric("w800", "speech", True, started_at)
        dashboard_event(
            "speech",
            "success",
            "语音识别与回读完成",
            f"session={session_id} samples={total_samples} tts_bytes={total_pcm_bytes}",
        )
    except AudioProtocolError as exc:
        if str(exc) != "Device disconnected during audio upload":
            with contextlib.suppress(ConnectionError, OSError):
                await send_audio_error(writer, session_id, exc.code, str(exc))
        dashboard_metric("w800", "speech", False, started_at)
        dashboard_event("speech", "warning", "语音协议失败", f"{exc.code}: {exc}")
        if exc.fatal:
            raise
    except SpeechPipelineError as exc:
        with contextlib.suppress(ConnectionError, OSError):
            await send_audio_error(writer, session_id, exc.code, str(exc))
        dashboard_metric("w800", "speech", False, started_at)
        dashboard_event("speech", "danger", "云端语音任务失败", f"{exc.code}: {exc}")
    except Exception as exc:
        logger.exception("unexpected speech session failure")
        with contextlib.suppress(ConnectionError, OSError):
            await send_audio_error(writer, session_id, "internal_error", "Speech service failed")
        dashboard_metric("w800", "speech", False, started_at)
        dashboard_event("speech", "danger", "语音服务异常", type(exc).__name__)
    finally:
        if asr_session is not None and not asr_finished:
            await asr_session.abort()


async def handle_client(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
    """处理一个 W800 TCP 连接。

    asyncio.start_server 每接入一个客户端，就会启动一次这个函数。
    你的项目里通常只有一个 W800 连接，但这个程序也能同时处理多个连接。
    每个连接都有独立的 history，所以互不串上下文。
    """
    peer = writer.get_extra_info("peername")
    logger.info("client connected: %s", peer)
    dashboard_event("network", "success", "W800 TCP连接已建立", "10086设备通道已连接")

    client = make_client()
    history: Deque[Dict[str, str]] = deque()

    try:
        while True:
            line = await reader.readline()
            if not line:
                break

            user_text = line.decode("utf-8", errors="replace").strip()
            if not user_text:
                continue
            started_at = time.perf_counter()
            operation = w800_metric_operation(user_text)

            if user_text.upper().startswith("FW1 "):
                await handle_fw1_command(reader, writer, user_text)
                dashboard_metric("w800", operation, True, started_at)
                continue

            if user_text.upper().startswith("AUD1 START "):
                try:
                    await handle_audio_session(reader, writer, user_text, client)
                except AudioProtocolError:
                    break
                continue

            if user_text.upper().startswith("AUD1 SAY "):
                try:
                    await handle_audio_say(reader, writer, user_text)
                except AudioProtocolError:
                    break
                continue

            # 简单自检命令：不调用 DeepSeek，只确认 TCP 服务收发正常。
            # 串口终端输入 /ping，如果收到 pong，说明：
            # 单片机 -> W800 -> 服务器 -> W800 -> 单片机 这条链路通了。
            if user_text.lower() in {"/ping", "ping"}:
                await send_text(writer, "pong\r\n[END]\r\n")
                dashboard_metric("w800", operation, True, started_at)
                continue

            # 冰箱状态上报：单片机每秒上报一次当前状态，服务器返回手机端期望的控制值。
            if user_text.upper().startswith("FRIDGE STATUS "):
                reply = await handle_fridge_status_command(user_text)
                await send_text(writer, f"{reply}\r\n[END]\r\n")
                dashboard_metric("w800", operation, " ERR " not in reply, started_at)
                continue

            # 药瓶仓储命令：MED RECOMMEND 会读取药箱后调用 DeepSeek，其它 MED 命令本地处理。
            if user_text.upper().startswith("HA "):
                reply = await handle_health_command(user_text, client)
                await send_text(writer, f"{reply}\r\n[END]\r\n")
                dashboard_metric("w800", operation, not reply.startswith("[HA] ERR"), started_at)
                continue

            if user_text.upper().startswith("MED "):
                reply = await handle_medicine_command(user_text, client)
                await send_text(writer, f"{reply}\r\n[END]\r\n")
                dashboard_metric("w800", operation, not reply.startswith("[MED] ERR"), started_at)
                continue

            logger.info("request from %s: %s", peer, user_text[:120])
            await send_text(writer, "[SERVER] DeepSeek request accepted, waiting...\r\n")

            try:
                reply = await ask_deepseek(client, history, user_text)
            except Exception as exc:  # Keep the MCU protocol alive even when the API fails.
                logger.exception("deepseek request failed")
                reply = f"[ERROR] DeepSeek 请求失败：{exc}"
                dashboard_event("ai", "danger", "通用AI请求失败", "DeepSeek调用异常")

            await send_text(writer, f"{reply}\r\n[END]\r\n")
            dashboard_metric("w800", operation, not reply.startswith("[ERROR]"), started_at)
    finally:
        logger.info("client disconnected: %s", peer)
        for device_id, device_writer in tuple(OTA_DEVICE_WRITERS.items()):
            if device_writer is writer:
                OTA_DEVICE_WRITERS.pop(device_id, None)
        dashboard_event("network", "warning", "W800 TCP连接已断开", "等待设备后台重连")
        writer.close()
        with contextlib.suppress(ConnectionError, OSError):
            await writer.wait_closed()


async def dashboard_sampler_loop() -> None:
    """Refresh device online state and persist one telemetry point every five seconds."""
    while True:
        try:
            async with fridge_lock:
                refresh_control_commands_unlocked()
                status = fridge_snapshot_unlocked()
            if dashboard_store is not None:
                dashboard_store.sample_status(status)
        except asyncio.CancelledError:
            raise
        except Exception:
            logger.exception("dashboard sampler failed")
        await asyncio.sleep(1.0)


async def main() -> None:
    """程序入口：启动 TCP 服务器并等待 systemd/用户停止。"""
    global dashboard_store

    load_medicine_records()
    logger.info("medicine db path=%s", MEDICINE_DB_PATH)
    dashboard_store = DashboardStore(
        DASHBOARD_DB_PATH,
        model=MODEL,
        tcp_port=PORT,
        http_port=HTTP_PORT,
    )
    if APP_TOKEN:
        dashboard_store.ensure_legacy_client(
            session_token_hash(APP_TOKEN),
            app_token_prefix(APP_TOKEN),
        )
    if not any(isinstance(handler, DashboardLogHandler) for handler in logger.handlers):
        logger.addHandler(DashboardLogHandler())
    FIRMWARE_ROOT.mkdir(parents=True, exist_ok=True)
    if not OTA_DEVICE_TOKEN:
        logger.warning("FRIDGE_OTA_TOKEN is not configured; FW1 requests will be rejected")
    if OTA_PUBLIC_KEY_PATH is None or not OTA_PUBLIC_KEY_PATH.is_file():
        logger.warning(
            "FRIDGE_FIRMWARE_PUBLIC_KEY is not configured or unreadable; OTA uploads will be rejected"
        )
    dashboard_event(
        "server",
        "success",
        "云端控制台服务已启动",
        f"HTTP {HTTP_PORT} / W800 {PORT}",
    )
    logger.info(
        "dashboard db path=%s, web root=%s, firmware root=%s",
        DASHBOARD_DB_PATH,
        DASHBOARD_WEB_ROOT,
        FIRMWARE_ROOT,
    )

    tcp_server = await asyncio.start_server(handle_client, HOST, PORT)
    http_server = await asyncio.start_server(handle_http_client, HTTP_HOST, HTTP_PORT)
    sampler_task = asyncio.create_task(dashboard_sampler_loop(), name="dashboard-sampler")
    tcp_sockets = ", ".join(str(sock.getsockname()) for sock in tcp_server.sockets or [])
    http_sockets = ", ".join(str(sock.getsockname()) for sock in http_server.sockets or [])
    logger.info("W800 TCP listening on %s, model=%s, base_url=%s", tcp_sockets, MODEL, BASE_URL)
    logger.info("Android HTTP API listening on %s", http_sockets)

    # systemd stop 或 Ctrl+C 时优雅退出。
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()
    for signame in ("SIGINT", "SIGTERM"):
        if hasattr(signal, signame):
            signum = getattr(signal, signame)
            try:
                loop.add_signal_handler(signum, stop_event.set)
            except NotImplementedError:  # Windows preview/test environment.
                signal.signal(signum, lambda *_args: loop.call_soon_threadsafe(stop_event.set))

    try:
        async with tcp_server, http_server:
            await stop_event.wait()
    finally:
        sampler_task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await sampler_task
        dashboard_event("server", "info", "云端控制台服务正在停止")
        if dashboard_store is not None:
            dashboard_store.close()
            dashboard_store = None


if __name__ == "__main__":
    asyncio.run(main())
