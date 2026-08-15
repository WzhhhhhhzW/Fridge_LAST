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
import json
import logging
import os
import signal
import time
from collections import deque
from pathlib import Path
from typing import Any, Deque, Dict, List
from urllib.parse import parse_qs, unquote, urlparse

from openai import AsyncOpenAI


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

# 药瓶仓储 JSON 保存文件。
# 默认保存在 /opt/fridge-deepseek 目录下，服务器重启后数据还在。
MEDICINE_DB_PATH = Path(os.getenv("MEDICINE_DB_PATH", "/opt/fridge-deepseek/medicine_store.json"))
MEDICINE_SLOT_COUNT = 32
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

fridge_state: Dict[str, Any] = {
    "online": False,
    "last_seen": 0.0,
    "set_temp": 1,
    "current_temp": 0.0,
    "fan": 0,
    "cool": False,
    "door": False,
    "vision_slot": None,
    "vision_name": "",
    "vision_updated_at": 0.0,
}

fridge_control: Dict[str, Any] = {
    "set_temp": 1,
    "fan": 0,
    "cool": False,
    # door 是一次性开门命令，发给单片机后会自动清零，避免舵机反复开门。
    "door": False,
}
fridge_control_initialized = False
fridge_control_dirty = False


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


def make_blank_medicine_record(slot: int) -> Dict[str, Any]:
    """创建一个空药瓶槽位。"""
    return {
        "valid": False,
        "slot": slot,
        "name": "",
        "production_date": "",
        "shelf_life": "",
        "efficacy": "",
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
    record["valid"] = valid
    for key in ("name", "production_date", "shelf_life", "efficacy"):
        value = raw.get(key, "")
        record[key] = str(value) if value is not None else ""
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


def validate_medicine_field(key: str, value: str, *, allow_empty: bool = False) -> str:
    """检查药品字段，避免协议分隔符和换行破坏单片机解析。"""
    value = value.strip()
    if not allow_empty and not value:
        raise ValueError(f"{key} is empty")
    if "|" in value or "\n" in value or "\r" in value:
        raise ValueError(f"{key} contains invalid character")
    if len(value) >= MEDICINE_FIELD_LIMITS[key]:
        raise ValueError(f"{key} is too long")
    return value


def split_medicine_fields(payload: str, field_count: int) -> List[str]:
    """按竖线分隔字段，并检查数量。"""
    fields = [field.strip() for field in payload.split("|")]
    if len(fields) != field_count:
        raise ValueError(f"need {field_count} fields")
    return fields


def format_medicine_record(record: Dict[str, Any]) -> str:
    """把一条记录格式化成单片机容易解析的一行。"""
    return (
        f"{int(record['slot'])}|{1 if record.get('valid') else 0}|"
        f"{record.get('name', '')}|{record.get('production_date', '')}|"
        f"{record.get('shelf_life', '')}|{record.get('efficacy', '')}"
    )


def medicine_ok(detail: str) -> str:
    return f"[MED] OK {detail}"


def medicine_err(reason: str) -> str:
    return f"[MED] ERR {reason}"


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


def fridge_snapshot_unlocked() -> Dict[str, Any]:
    now = time.time()
    online = (now - float(fridge_state.get("last_seen", 0.0))) <= FRIDGE_ONLINE_TIMEOUT_SECONDS
    fridge_state["online"] = online
    snapshot = dict(fridge_state)
    snapshot["last_seen_age_seconds"] = None if not fridge_state["last_seen"] else round(now - fridge_state["last_seen"], 1)
    return snapshot


def medicine_record_for_api(record: Dict[str, Any]) -> Dict[str, Any]:
    return {
        "valid": bool(record.get("valid")),
        "slot": int(record.get("slot", 0)),
        "name": str(record.get("name", "")),
        "production_date": str(record.get("production_date", "")),
        "shelf_life": str(record.get("shelf_life", "")),
        "efficacy": str(record.get("efficacy", "")),
    }


def medicine_name_for_slot(records: List[Dict[str, Any]], slot: int | None) -> str:
    if slot is None or slot < 0 or slot >= MEDICINE_SLOT_COUNT:
        return ""
    record = records[slot]
    if not record.get("valid"):
        return "未录入药品"
    return str(record.get("name", ""))


async def handle_fridge_status_command(user_text: str) -> str:
    """处理 MCU 每秒上报的冰箱状态，并返回手机端设置的目标值。"""
    global fridge_control_initialized, fridge_control_dirty

    payload = user_text[len("FRIDGE STATUS "):].strip()
    fields = [field.strip() for field in payload.split("|")]
    if len(fields) != 6:
        return "[FRIDGE] ERR need 6 fields"

    try:
        set_temp_value = clamp_int(int(fields[0], 10), -40, 80)
        temp_x10 = int(fields[1], 10)
        fan_value = clamp_int(int(fields[2], 10), 0, 2)
        cool_value = parse_bool_value(fields[3])
        door_value = parse_bool_value(fields[4])
        vision_slot_raw = int(fields[5], 10)
        vision_slot = None if vision_slot_raw == VISION_SLOT_NONE else parse_slot(str(vision_slot_raw))
    except ValueError as exc:
        return f"[FRIDGE] ERR {exc}"

    async with medicine_lock:
        records = load_medicine_records()

    async with fridge_lock:
        now = time.time()
        fridge_state.update(
            {
                "online": True,
                "last_seen": now,
                "set_temp": set_temp_value,
                "current_temp": round(temp_x10 / 10.0, 1),
                "fan": fan_value,
                "cool": cool_value,
                "door": door_value,
            }
        )

        if vision_slot is not None:
            fridge_state["vision_slot"] = vision_slot
            fridge_state["vision_name"] = medicine_name_for_slot(records, vision_slot)
            fridge_state["vision_updated_at"] = now

        if not fridge_control_initialized:
            if not fridge_control_dirty:
                fridge_control["set_temp"] = set_temp_value
                fridge_control["fan"] = fan_value
                fridge_control["cool"] = cool_value
                fridge_control["door"] = False
            fridge_control_initialized = True

        target_set_temp = clamp_int(int(fridge_control["set_temp"]), -40, 80)
        target_fan = clamp_int(int(fridge_control["fan"]), 0, 2)
        target_cool = 1 if bool(fridge_control["cool"]) else 0

        if fridge_control_dirty:
            if (
                set_temp_value == target_set_temp
                and fan_value == target_fan
                and cool_value == bool(fridge_control["cool"])
            ):
                fridge_control_dirty = False
        else:
            fridge_control["set_temp"] = set_temp_value
            fridge_control["fan"] = fan_value
            fridge_control["cool"] = cool_value
            target_set_temp = set_temp_value
            target_fan = fan_value
            target_cool = 1 if cool_value else 0

        target_door = 1 if bool(fridge_control["door"]) else 0
        fridge_control["door"] = False

    return f"[FRIDGE] OK {target_set_temp}|{target_fan}|{target_cool}|{target_door}"


async def ask_medicine_recommendation(client: AsyncOpenAI,
                                      symptom: str,
                                      records: List[Dict[str, Any]]) -> str:
    """让 DeepSeek 只在药箱已有药品里选一个药名。"""
    valid_records = [record for record in records if record.get("valid") and record.get("name")]
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

            if op == "SET":
                slot_text, name, production_date, shelf_life, efficacy = split_medicine_fields(payload, 5)
                slot = parse_slot(slot_text)
                records[slot] = {
                    "valid": True,
                    "slot": slot,
                    "name": validate_medicine_field("name", name),
                    "production_date": validate_medicine_field("production_date", production_date),
                    "shelf_life": validate_medicine_field("shelf_life", shelf_life),
                    "efficacy": validate_medicine_field("efficacy", efficacy),
                }
                save_medicine_records(records)
                logger.info("medicine SET slot=%s name=%s", slot, records[slot].get("name", ""))
                return medicine_ok(f"SET {slot}")

            if op == "ADD":
                name, production_date, shelf_life, efficacy = split_medicine_fields(payload, 4)
                empty_slots = [record["slot"] for record in records if not record.get("valid")]
                if not empty_slots:
                    return medicine_err("FULL")

                slot = int(empty_slots[0])
                records[slot] = {
                    "valid": True,
                    "slot": slot,
                    "name": validate_medicine_field("name", name),
                    "production_date": validate_medicine_field("production_date", production_date),
                    "shelf_life": validate_medicine_field("shelf_life", shelf_life),
                    "efficacy": validate_medicine_field("efficacy", efficacy),
                }
                save_medicine_records(records)
                logger.info("medicine ADD slot=%s name=%s", slot, records[slot].get("name", ""))
                return medicine_ok(f"ADD {slot}")

            if op == "DEL":
                old_name = records[slot].get("name", "")
                records[slot] = make_blank_medicine_record(slot)
                save_medicine_records(records)
                logger.info("medicine DEL slot=%s old_name=%s", slot, old_name)
                return medicine_ok(f"DEL {slot}")

            if op == "LIST":
                items = [
                    f"{record['slot']}:{record.get('name', '')}"
                    for record in records
                    if record.get("valid")
                ]
                return medicine_ok("LIST " + (";".join(items) if items else "EMPTY"))

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


async def send_http_json(writer: asyncio.StreamWriter, status: int, payload: Dict[str, Any]) -> None:
    reason = {
        200: "OK",
        201: "Created",
        204: "No Content",
        400: "Bad Request",
        401: "Unauthorized",
        404: "Not Found",
        405: "Method Not Allowed",
        500: "Internal Server Error",
    }.get(status, "OK")
    body = b"" if status == 204 else make_json_body(payload)
    headers = [
        f"HTTP/1.1 {status} {reason}",
        "Content-Type: application/json; charset=utf-8",
        f"Content-Length: {len(body)}",
        "Connection: close",
        "Access-Control-Allow-Origin: *",
        "Access-Control-Allow-Headers: Content-Type, X-Fridge-Token",
        "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS",
        "",
        "",
    ]
    writer.write("\r\n".join(headers).encode("ascii") + body)
    await writer.drain()


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


async def handle_http_api(method: str,
                          target: str,
                          headers: Dict[str, str],
                          body: bytes) -> tuple[int, Dict[str, Any]]:
    global fridge_control_dirty

    parsed = urlparse(target)
    path = unquote(parsed.path)
    query = parse_qs(parsed.query)

    if method == "OPTIONS":
        return 204, {}

    if not APP_TOKEN or headers.get("x-fridge-token", "") != APP_TOKEN:
        return 401, {"ok": False, "error": "unauthorized"}

    try:
        if path == "/api/ping" and method == "GET":
            return 200, {"ok": True, "message": "pong"}

        if path == "/api/fridge/status" and method == "GET":
            async with fridge_lock:
                return 200, {
                    "ok": True,
                    "status": fridge_snapshot_unlocked(),
                    "control": dict(fridge_control),
                    "control_pending": fridge_control_dirty,
                }

        if path == "/api/fridge/control" and method == "POST":
            payload = parse_json_request(body)
            async with fridge_lock:
                control_changed = False
                if "set_temp" in payload:
                    fridge_control["set_temp"] = clamp_int(int(payload["set_temp"]), -40, 80)
                    control_changed = True
                if "fan" in payload:
                    fridge_control["fan"] = clamp_int(int(payload["fan"]), 0, 2)
                    control_changed = True
                if "cool" in payload:
                    fridge_control["cool"] = parse_bool_value(payload["cool"])
                    control_changed = True
                if "door" in payload:
                    fridge_control["door"] = parse_bool_value(payload["door"])
                if control_changed:
                    fridge_control_dirty = True
                return 200, {
                    "ok": True,
                    "control": dict(fridge_control),
                    "control_pending": fridge_control_dirty,
                    "status": fridge_snapshot_unlocked(),
                }

        if path == "/api/vision/latest" and method == "GET":
            async with fridge_lock:
                return 200, {
                    "ok": True,
                    "vision": {
                        "slot": fridge_state.get("vision_slot"),
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
            async with medicine_lock:
                records = load_medicine_records()
                empty_slots = [record["slot"] for record in records if not record.get("valid")]
                if not empty_slots:
                    return 400, {"ok": False, "error": "FULL"}
                slot = int(empty_slots[0])
                records[slot] = {
                    "valid": True,
                    "slot": slot,
                    "name": validate_medicine_field("name", json_field(payload, "name")),
                    "production_date": validate_medicine_field("production_date", json_field(payload, "production_date")),
                    "shelf_life": validate_medicine_field("shelf_life", json_field(payload, "shelf_life")),
                    "efficacy": validate_medicine_field("efficacy", json_field(payload, "efficacy")),
                }
                save_medicine_records(records)
                logger.info("HTTP medicine ADD slot=%s name=%s", slot, records[slot].get("name", ""))
                return 201, {"ok": True, "record": medicine_record_for_api(records[slot])}

        if path.startswith("/api/medicines/"):
            slot = parse_slot(path.removeprefix("/api/medicines/"))

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
                    records[slot] = {
                        "valid": True,
                        "slot": slot,
                        "name": validate_medicine_field("name", json_field(payload, "name")),
                        "production_date": validate_medicine_field("production_date", json_field(payload, "production_date")),
                        "shelf_life": validate_medicine_field("shelf_life", json_field(payload, "shelf_life")),
                        "efficacy": validate_medicine_field("efficacy", json_field(payload, "efficacy")),
                    }
                    save_medicine_records(records)
                    logger.info("HTTP medicine SET slot=%s name=%s", slot, records[slot].get("name", ""))
                    return 200, {"ok": True, "record": medicine_record_for_api(records[slot])}

            if method == "DELETE":
                async with medicine_lock:
                    records = load_medicine_records()
                    old_name = records[slot].get("name", "")
                    records[slot] = make_blank_medicine_record(slot)
                    save_medicine_records(records)
                    logger.info("HTTP medicine DEL slot=%s old_name=%s", slot, old_name)
                return 200, {"ok": True, "slot": slot}

            return 405, {"ok": False, "error": "method not allowed"}

        if path == "/api/health/recommend" and method == "POST":
            payload = parse_json_request(body)
            symptom = validate_symptom_field(json_field(payload, "symptom"))
            async with medicine_lock:
                records = load_medicine_records()
            result = await ask_medicine_recommendation(make_client(), symptom, records)
            return 200, {"ok": True, "result": result}

        return 404, {"ok": False, "error": "not found"}
    except ValueError as exc:
        return 400, {"ok": False, "error": str(exc)}
    except json.JSONDecodeError as exc:
        return 400, {"ok": False, "error": f"invalid JSON: {exc}"}
    except Exception as exc:
        logger.exception("HTTP API failed: %s %s", method, path)
        return 500, {"ok": False, "error": str(exc)}


async def handle_http_client(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
    peer = writer.get_extra_info("peername")
    try:
        raw_headers = await reader.readuntil(b"\r\n\r\n")
        method, target, headers = decode_http_headers(raw_headers[:-4])
        content_length = int(headers.get("content-length", "0") or "0")
        if content_length > 64 * 1024:
            await send_http_json(writer, 400, {"ok": False, "error": "request body too large"})
            return

        body = await reader.readexactly(content_length) if content_length > 0 else b""
        status, payload = await handle_http_api(method, target, headers, body)
        await send_http_json(writer, status, payload)
    except Exception as exc:
        logger.warning("bad HTTP request from %s: %s", peer, exc)
        await send_http_json(writer, 400, {"ok": False, "error": "bad request"})
    finally:
        writer.close()
        await writer.wait_closed()


async def handle_client(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
    """处理一个 W800 TCP 连接。

    asyncio.start_server 每接入一个客户端，就会启动一次这个函数。
    你的项目里通常只有一个 W800 连接，但这个程序也能同时处理多个连接。
    每个连接都有独立的 history，所以互不串上下文。
    """
    peer = writer.get_extra_info("peername")
    logger.info("client connected: %s", peer)

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

            # 简单自检命令：不调用 DeepSeek，只确认 TCP 服务收发正常。
            # 串口终端输入 /ping，如果收到 pong，说明：
            # 单片机 -> W800 -> 服务器 -> W800 -> 单片机 这条链路通了。
            if user_text.lower() in {"/ping", "ping"}:
                await send_text(writer, "pong\r\n[END]\r\n")
                continue

            # 冰箱状态上报：单片机每秒上报一次当前状态，服务器返回手机端期望的控制值。
            if user_text.upper().startswith("FRIDGE STATUS "):
                reply = await handle_fridge_status_command(user_text)
                await send_text(writer, f"{reply}\r\n[END]\r\n")
                continue

            # 药瓶仓储命令：MED RECOMMEND 会读取药箱后调用 DeepSeek，其它 MED 命令本地处理。
            if user_text.upper().startswith("MED "):
                reply = await handle_medicine_command(user_text, client)
                await send_text(writer, f"{reply}\r\n[END]\r\n")
                continue

            logger.info("request from %s: %s", peer, user_text[:120])
            await send_text(writer, "[SERVER] DeepSeek request accepted, waiting...\r\n")

            try:
                reply = await ask_deepseek(client, history, user_text)
            except Exception as exc:  # Keep the MCU protocol alive even when the API fails.
                logger.exception("deepseek request failed")
                reply = f"[ERROR] DeepSeek 请求失败：{exc}"

            await send_text(writer, f"{reply}\r\n[END]\r\n")
    finally:
        logger.info("client disconnected: %s", peer)
        writer.close()
        await writer.wait_closed()


async def main() -> None:
    """程序入口：启动 TCP 服务器并等待 systemd/用户停止。"""
    load_medicine_records()
    logger.info("medicine db path=%s", MEDICINE_DB_PATH)

    tcp_server = await asyncio.start_server(handle_client, HOST, PORT)
    http_server = await asyncio.start_server(handle_http_client, HTTP_HOST, HTTP_PORT)
    tcp_sockets = ", ".join(str(sock.getsockname()) for sock in tcp_server.sockets or [])
    http_sockets = ", ".join(str(sock.getsockname()) for sock in http_server.sockets or [])
    logger.info("W800 TCP listening on %s, model=%s, base_url=%s", tcp_sockets, MODEL, BASE_URL)
    logger.info("Android HTTP API listening on %s", http_sockets)

    # systemd stop 或 Ctrl+C 时优雅退出。
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()
    for signame in ("SIGINT", "SIGTERM"):
        if hasattr(signal, signame):
            loop.add_signal_handler(getattr(signal, signame), stop_event.set)

    async with tcp_server, http_server:
        await stop_event.wait()


if __name__ == "__main__":
    asyncio.run(main())
