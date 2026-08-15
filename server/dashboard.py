"""Persistent telemetry, admin sessions and audit data for the fridge server."""

from __future__ import annotations

import calendar
import math
import re
import sqlite3
import statistics
import time
from collections import deque
from datetime import date, datetime, timedelta
from pathlib import Path
from typing import Any, Dict, Iterable, List

try:
    import psutil
except ImportError:  # The production requirements install psutil; keep local tools usable.
    psutil = None  # type: ignore[assignment]


TELEMETRY_RETENTION_SECONDS = 7 * 24 * 60 * 60
EVENT_RETENTION_SECONDS = 30 * 24 * 60 * 60
HEALTH_RETENTION_SECONDS = 30 * 24 * 60 * 60
METRIC_RETENTION_SECONDS = 90 * 24 * 60 * 60
TELEMETRY_SAMPLE_SECONDS = 5.0
MAX_EVENT_ROWS = 10_000
MAX_HEALTH_ROWS = 1_000
AUDIT_RETENTION_SECONDS = 90 * 24 * 60 * 60
MAX_AUDIT_ROWS = 20_000
LOG_RETENTION_SECONDS = 30 * 24 * 60 * 60
MAX_LOG_ROWS = 20_000
CLIENT_ONLINE_SECONDS = 60.0
DEFAULT_POWER_LIMIT_W = 200.0
POWER_BASELINE_SAMPLES = 60
POWER_BASELINE_MIN_SAMPLES = 20
POWER_STARTUP_GRACE_SECONDS = 10.0
POWER_ALERT_COOLDOWN_SECONDS = 60.0


def sanitize_summary(value: str, limit: int = 60) -> str:
    """Collapse whitespace and keep only a short, display-safe health summary."""
    collapsed = re.sub(r"\s+", " ", str(value or "")).strip()
    return collapsed[:limit]


def _add_months(value: date, months: int) -> date:
    month_index = value.month - 1 + months
    year = value.year + month_index // 12
    month = month_index % 12 + 1
    day = min(value.day, calendar.monthrange(year, month)[1])
    return date(year, month, day)


def normalize_production_date(value: str) -> str:
    """Accept common Chinese date forms and return an ISO calendar date."""
    text = str(value or "").strip()
    match = re.fullmatch(r"(\d{4})\s*(?:年|[-/.])\s*(\d{1,2})\s*(?:月|[-/.])\s*(\d{1,2})\s*日?", text)
    if not match:
        raise ValueError("生产日期格式应为2025-07-16或2025年7月16日")
    try:
        produced = date(int(match.group(1)), int(match.group(2)), int(match.group(3)))
    except ValueError as exc:
        raise ValueError("生产日期不是有效日期") from exc
    return produced.isoformat()


def _average_optional(rows: Iterable[sqlite3.Row], key: str) -> float | None:
    values = [float(row[key]) for row in rows if row[key] is not None]
    return round(sum(values) / len(values), 2) if values else None


def medicine_expiry(production_date: str, shelf_life: str, *, today: date | None = None) -> Dict[str, Any]:
    """Return expiry status for shelf-life strings such as 30天, 12个月 or 2年."""
    today = today or datetime.now().astimezone().date()
    try:
        produced = date.fromisoformat(normalize_production_date(production_date))
    except (TypeError, ValueError):
        return {"expiry_date": None, "expiry_status": "unknown", "days_remaining": None}

    match = re.fullmatch(r"\s*(\d+)\s*(天|个月|月|年)\s*", str(shelf_life))
    if not match:
        return {"expiry_date": None, "expiry_status": "unknown", "days_remaining": None}

    amount = int(match.group(1))
    unit = match.group(2)
    if unit == "天":
        expires = produced + timedelta(days=amount)
    elif unit in {"个月", "月"}:
        expires = _add_months(produced, amount)
    else:
        expires = _add_months(produced, amount * 12)

    days_remaining = (expires - today).days
    if days_remaining < 0:
        status = "expired"
    elif days_remaining <= 30:
        status = "expiring"
    else:
        status = "normal"
    return {
        "expiry_date": expires.isoformat(),
        "expiry_status": status,
        "days_remaining": days_remaining,
    }


class DashboardStore:
    """SQLite-backed dashboard telemetry, events and aggregate request metrics."""

    def __init__(self,
                 db_path: Path,
                 *,
                 model: str,
                 tcp_port: int,
                 http_port: int) -> None:
        self.db_path = Path(db_path)
        self.db_path.parent.mkdir(parents=True, exist_ok=True)
        self.connection = sqlite3.connect(self.db_path, timeout=5.0, check_same_thread=False)
        self.connection.row_factory = sqlite3.Row
        self.connection.execute("PRAGMA journal_mode=WAL")
        self.connection.execute("PRAGMA synchronous=NORMAL")
        self.connection.execute("PRAGMA busy_timeout=5000")
        self.connection.execute("PRAGMA foreign_keys=ON")
        self.model = model
        self.tcp_port = tcp_port
        self.http_port = http_port
        self.started_at = time.time()
        self.last_sample_at = 0.0
        self.last_cleanup_at = 0.0
        self.last_online: bool | None = None
        self.last_vision_code: int | None = None
        self.power_baseline = deque(maxlen=POWER_BASELINE_SAMPLES)
        self.cool_started_at: float | None = None
        self.power_normal_streak = 0
        self.process = psutil.Process() if psutil is not None else None
        if psutil is not None:
            psutil.cpu_percent(interval=None)
        self._create_schema()
        latest_vision = self.connection.execute(
            "SELECT code FROM vision_recognitions ORDER BY last_seen DESC LIMIT 1"
        ).fetchone()
        if latest_vision is not None:
            self.last_vision_code = int(latest_vision["code"])

    def _create_schema(self) -> None:
        self.connection.executescript(
            """
            CREATE TABLE IF NOT EXISTS telemetry (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                ts REAL NOT NULL,
                online INTEGER NOT NULL,
                set_temp REAL NOT NULL,
                current_temp REAL NOT NULL,
                fan INTEGER NOT NULL,
                cool INTEGER NOT NULL,
                door INTEGER NOT NULL,
                vision_slot INTEGER,
                vision_name TEXT NOT NULL DEFAULT ''
            );
            CREATE INDEX IF NOT EXISTS idx_telemetry_ts ON telemetry(ts);

            CREATE TABLE IF NOT EXISTS events (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                ts REAL NOT NULL,
                category TEXT NOT NULL,
                severity TEXT NOT NULL,
                title TEXT NOT NULL,
                detail TEXT NOT NULL DEFAULT ''
            );
            CREATE INDEX IF NOT EXISTS idx_events_ts ON events(ts DESC);

            CREATE TABLE IF NOT EXISTS vision_recognitions (
                code INTEGER PRIMARY KEY,
                medicine_name TEXT NOT NULL,
                last_seen REAL NOT NULL,
                recognition_count INTEGER NOT NULL DEFAULT 1,
                include_in_recent INTEGER NOT NULL DEFAULT 1
            );
            CREATE INDEX IF NOT EXISTS idx_vision_recognitions_last_seen
                ON vision_recognitions(last_seen DESC);

            CREATE TABLE IF NOT EXISTS health_records (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                ts REAL NOT NULL,
                symptom_summary TEXT NOT NULL,
                diagnosis TEXT NOT NULL,
                medicine TEXT NOT NULL,
                success INTEGER NOT NULL,
                latency_ms REAL NOT NULL
            );
            CREATE INDEX IF NOT EXISTS idx_health_ts ON health_records(ts DESC);

            CREATE TABLE IF NOT EXISTS metric_buckets (
                bucket_ts INTEGER NOT NULL,
                channel TEXT NOT NULL,
                operation TEXT NOT NULL,
                request_count INTEGER NOT NULL,
                success_count INTEGER NOT NULL,
                total_latency_ms REAL NOT NULL,
                max_latency_ms REAL NOT NULL,
                PRIMARY KEY (bucket_ts, channel, operation)
            );

            CREATE TABLE IF NOT EXISTS admin_sessions (
                session_hash TEXT PRIMARY KEY,
                csrf_token TEXT NOT NULL,
                created_at REAL NOT NULL,
                expires_at REAL NOT NULL,
                last_seen REAL NOT NULL
            );
            CREATE INDEX IF NOT EXISTS idx_admin_sessions_expires
                ON admin_sessions(expires_at);

            CREATE TABLE IF NOT EXISTS control_audit (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                ts REAL NOT NULL,
                actor TEXT NOT NULL,
                category TEXT NOT NULL,
                action TEXT NOT NULL,
                target TEXT NOT NULL DEFAULT '',
                summary TEXT NOT NULL DEFAULT '',
                result TEXT NOT NULL,
                command_id TEXT,
                confirmed_at REAL
            );
            CREATE INDEX IF NOT EXISTS idx_control_audit_ts
                ON control_audit(ts DESC);
            CREATE INDEX IF NOT EXISTS idx_control_audit_command
                ON control_audit(command_id);

            CREATE TABLE IF NOT EXISTS phone_clients (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                name TEXT NOT NULL,
                token_hash TEXT NOT NULL UNIQUE,
                token_prefix TEXT NOT NULL DEFAULT '',
                notes TEXT NOT NULL DEFAULT '',
                enabled INTEGER NOT NULL DEFAULT 1,
                is_legacy INTEGER NOT NULL DEFAULT 0,
                created_at REAL NOT NULL,
                last_seen REAL,
                disabled_at REAL
            );
            CREATE INDEX IF NOT EXISTS idx_phone_clients_last_seen
                ON phone_clients(last_seen DESC);

            CREATE TABLE IF NOT EXISTS power_settings (
                id INTEGER PRIMARY KEY CHECK (id = 1),
                absolute_limit_w REAL NOT NULL,
                relative_multiplier REAL NOT NULL,
                relative_step_w REAL NOT NULL,
                updated_at REAL NOT NULL
            );

            CREATE TABLE IF NOT EXISTS power_alerts (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                started_at REAL NOT NULL,
                updated_at REAL NOT NULL,
                ended_at REAL,
                status TEXT NOT NULL,
                power_w REAL NOT NULL,
                baseline_w REAL,
                threshold_w REAL NOT NULL,
                detail TEXT NOT NULL DEFAULT '',
                acknowledged INTEGER NOT NULL DEFAULT 0,
                acknowledged_at REAL
            );
            CREATE INDEX IF NOT EXISTS idx_power_alerts_started
                ON power_alerts(started_at DESC);

            CREATE TABLE IF NOT EXISTS application_logs (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                ts REAL NOT NULL,
                level TEXT NOT NULL,
                module TEXT NOT NULL,
                message TEXT NOT NULL
            );
            CREATE INDEX IF NOT EXISTS idx_application_logs_ts
                ON application_logs(ts DESC);

            CREATE TABLE IF NOT EXISTS firmware_releases (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                component TEXT NOT NULL,
                version TEXT NOT NULL,
                title TEXT NOT NULL,
                changelog TEXT NOT NULL DEFAULT '',
                channel TEXT NOT NULL DEFAULT 'stable',
                published INTEGER NOT NULL DEFAULT 0,
                is_current INTEGER NOT NULL DEFAULT 0,
                created_at REAL NOT NULL,
                updated_at REAL NOT NULL,
                UNIQUE(component, version)
            );
            CREATE INDEX IF NOT EXISTS idx_firmware_releases_component
                ON firmware_releases(component, published, created_at DESC);

            CREATE TABLE IF NOT EXISTS firmware_artifacts (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                release_id INTEGER NOT NULL,
                kind TEXT NOT NULL,
                filename TEXT NOT NULL,
                stored_name TEXT NOT NULL,
                size_bytes INTEGER NOT NULL,
                sha256 TEXT NOT NULL,
                downloads INTEGER NOT NULL DEFAULT 0,
                created_at REAL NOT NULL,
                FOREIGN KEY(release_id) REFERENCES firmware_releases(id) ON DELETE CASCADE,
                UNIQUE(release_id, kind, sha256)
            );
            CREATE INDEX IF NOT EXISTS idx_firmware_artifacts_release
                ON firmware_artifacts(release_id);

            CREATE TABLE IF NOT EXISTS ota_devices (
                device_id TEXT PRIMARY KEY,
                part_number TEXT NOT NULL,
                app_version TEXT NOT NULL,
                boot_version TEXT NOT NULL,
                active_bank INTEGER NOT NULL,
                ota_state TEXT NOT NULL DEFAULT 'idle',
                received_bytes INTEGER NOT NULL DEFAULT 0,
                total_bytes INTEGER NOT NULL DEFAULT 0,
                current_deployment_id TEXT,
                last_result TEXT NOT NULL DEFAULT '',
                last_error TEXT NOT NULL DEFAULT '',
                last_seen REAL NOT NULL,
                updated_at REAL NOT NULL
            );
            CREATE INDEX IF NOT EXISTS idx_ota_devices_last_seen
                ON ota_devices(last_seen DESC);

            CREATE TABLE IF NOT EXISTS ota_deployments (
                id TEXT PRIMARY KEY,
                device_id TEXT NOT NULL,
                artifact_id INTEGER NOT NULL,
                from_version TEXT NOT NULL,
                target_version TEXT NOT NULL,
                status TEXT NOT NULL,
                bytes_total INTEGER NOT NULL,
                bytes_sent INTEGER NOT NULL DEFAULT 0,
                error_code TEXT NOT NULL DEFAULT '',
                requested_by TEXT NOT NULL,
                created_at REAL NOT NULL,
                started_at REAL,
                updated_at REAL NOT NULL,
                completed_at REAL
            );
            CREATE INDEX IF NOT EXISTS idx_ota_deployments_device
                ON ota_deployments(device_id, created_at DESC);
            CREATE INDEX IF NOT EXISTS idx_ota_deployments_status
                ON ota_deployments(status, updated_at DESC);
            """
        )
        self._ensure_column("telemetry", "power_40_w", "REAL")
        self._ensure_column("telemetry", "power_41_w", "REAL")
        self._ensure_column("telemetry", "power_total_w", "REAL")
        self._ensure_column("telemetry", "network_rtt_ms", "REAL")
        self._ensure_column("telemetry", "heartbeat_gap_ms", "REAL")
        self._ensure_column("firmware_artifacts", "target_part", "TEXT NOT NULL DEFAULT ''")
        self._ensure_column("firmware_artifacts", "image_version", "TEXT NOT NULL DEFAULT ''")
        self._ensure_column("firmware_artifacts", "min_bootloader", "TEXT NOT NULL DEFAULT ''")
        self._ensure_column("firmware_artifacts", "signature_type", "TEXT NOT NULL DEFAULT ''")
        self._ensure_column("firmware_artifacts", "image_valid", "INTEGER NOT NULL DEFAULT 0")
        self._ensure_column("firmware_artifacts", "validation_error", "TEXT NOT NULL DEFAULT ''")
        self.connection.execute(
            """
            INSERT OR IGNORE INTO power_settings(
                id, absolute_limit_w, relative_multiplier, relative_step_w, updated_at
            ) VALUES (1, ?, 1.5, 10.0, ?)
            """,
            (DEFAULT_POWER_LIMIT_W, time.time()),
        )
        self.connection.commit()

    def _ensure_column(self, table: str, column: str, declaration: str) -> None:
        columns = {
            str(row["name"])
            for row in self.connection.execute(f"PRAGMA table_info({table})").fetchall()
        }
        if column not in columns:
            self.connection.execute(f"ALTER TABLE {table} ADD COLUMN {column} {declaration}")

    def close(self) -> None:
        self.connection.close()

    def add_event(self,
                  category: str,
                  severity: str,
                  title: str,
                  detail: str = "",
                  *,
                  timestamp: float | None = None) -> None:
        self.connection.execute(
            "INSERT INTO events(ts, category, severity, title, detail) VALUES (?, ?, ?, ?, ?)",
            (
                timestamp or time.time(),
                str(category)[:32],
                str(severity)[:16],
                sanitize_summary(title, 80),
                sanitize_summary(detail, 240),
            ),
        )
        self.connection.commit()

    def record_vision_recognition(self,
                                  code: int,
                                  medicine_name: str,
                                  *,
                                  include_in_recent: bool = True,
                                  timestamp: float | None = None) -> bool:
        """Persist one visual class transition and suppress heartbeat repeats."""
        vision_code = int(code)
        if self.last_vision_code == vision_code:
            return False

        seen_at = float(timestamp or time.time())
        self.last_vision_code = vision_code
        self.connection.execute(
            """
            INSERT INTO vision_recognitions(
                code, medicine_name, last_seen, recognition_count, include_in_recent
            ) VALUES (?, ?, ?, 1, ?)
            ON CONFLICT(code) DO UPDATE SET
                medicine_name = excluded.medicine_name,
                last_seen = excluded.last_seen,
                recognition_count = vision_recognitions.recognition_count + 1,
                include_in_recent = excluded.include_in_recent
            """,
            (
                vision_code,
                sanitize_summary(medicine_name, 80),
                seen_at,
                1 if include_in_recent else 0,
            ),
        )
        self.connection.commit()
        return True

    def recent_vision_recognitions(self, limit: int = 5) -> List[Dict[str, Any]]:
        safe_limit = max(1, min(int(limit), 5))
        rows = self.connection.execute(
            """
            SELECT code, medicine_name AS name, last_seen
            FROM vision_recognitions
            WHERE include_in_recent = 1
            ORDER BY last_seen DESC
            LIMIT ?
            """,
            (safe_limit,),
        ).fetchall()
        return [dict(row) for row in rows]

    def add_health(self,
                   symptom: str,
                   diagnosis: str,
                   medicine: str,
                   success: bool,
                   latency_ms: float) -> None:
        self.connection.execute(
            """
            INSERT INTO health_records(ts, symptom_summary, diagnosis, medicine, success, latency_ms)
            VALUES (?, ?, ?, ?, ?, ?)
            """,
            (
                time.time(),
                sanitize_summary(symptom),
                sanitize_summary(diagnosis, 48),
                sanitize_summary(medicine, 80),
                1 if success else 0,
                round(max(0.0, latency_ms), 1),
            ),
        )
        self.connection.commit()

    def create_admin_session(self,
                             session_hash: str,
                             csrf_token: str,
                             expires_at: float,
                             *,
                             created_at: float | None = None) -> None:
        """Persist only the hash of a browser session token."""
        now = created_at or time.time()
        self.connection.execute(
            """
            INSERT OR REPLACE INTO admin_sessions(
                session_hash, csrf_token, created_at, expires_at, last_seen
            ) VALUES (?, ?, ?, ?, ?)
            """,
            (str(session_hash), str(csrf_token), now, float(expires_at), now),
        )
        self.connection.commit()

    def admin_session(self,
                      session_hash: str,
                      *,
                      now: float | None = None,
                      touch: bool = True) -> Dict[str, Any] | None:
        now = now or time.time()
        row = self.connection.execute(
            """
            SELECT session_hash, csrf_token, created_at, expires_at, last_seen
            FROM admin_sessions WHERE session_hash = ?
            """,
            (str(session_hash),),
        ).fetchone()
        if row is None:
            return None
        if float(row["expires_at"]) <= now:
            self.delete_admin_session(session_hash)
            return None
        if touch:
            self.connection.execute(
                "UPDATE admin_sessions SET last_seen = ? WHERE session_hash = ?",
                (now, str(session_hash)),
            )
            self.connection.commit()
        return dict(row)

    def delete_admin_session(self, session_hash: str) -> None:
        self.connection.execute(
            "DELETE FROM admin_sessions WHERE session_hash = ?",
            (str(session_hash),),
        )
        self.connection.commit()

    def add_audit(self,
                  actor: str,
                  category: str,
                  action: str,
                  *,
                  target: str = "",
                  summary: str = "",
                  result: str = "accepted",
                  command_id: str | None = None,
                  timestamp: float | None = None) -> int:
        cursor = self.connection.execute(
            """
            INSERT INTO control_audit(
                ts, actor, category, action, target, summary, result, command_id
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (
                timestamp or time.time(),
                sanitize_summary(actor, 32),
                sanitize_summary(category, 32),
                sanitize_summary(action, 48),
                sanitize_summary(target, 80),
                sanitize_summary(summary, 240),
                sanitize_summary(result, 24),
                sanitize_summary(command_id or "", 40) or None,
            ),
        )
        self.connection.commit()
        return int(cursor.lastrowid)

    def update_audit_command(self,
                             command_id: str,
                             result: str,
                             *,
                             confirmed_at: float | None = None) -> None:
        if not command_id:
            return
        self.connection.execute(
            """
            UPDATE control_audit
            SET result = ?, confirmed_at = COALESCE(?, confirmed_at)
            WHERE command_id = ?
            """,
            (sanitize_summary(result, 24), confirmed_at, str(command_id)),
        )
        self.connection.commit()

    def audit(self, limit: int) -> List[Dict[str, Any]]:
        safe_limit = max(1, min(int(limit), 200))
        rows = self.connection.execute(
            """
            SELECT id, ts, actor, category, action, target, summary,
                   result, command_id, confirmed_at
            FROM control_audit ORDER BY id DESC LIMIT ?
            """,
            (safe_limit,),
        ).fetchall()
        return [dict(row) for row in rows]

    def ensure_legacy_client(self, token_hash: str, token_prefix: str) -> Dict[str, Any]:
        row = self.client_by_token_hash(token_hash)
        if row is None:
            now = time.time()
            self.connection.execute(
                """
                INSERT INTO phone_clients(
                    name, token_hash, token_prefix, notes, enabled, is_legacy, created_at
                ) VALUES (?, ?, ?, ?, 1, 1, ?)
                """,
                ("旧版共享客户端", token_hash, token_prefix, "FRIDGE_APP_TOKEN兼容入口", now),
            )
            self.connection.commit()
            row = self.client_by_token_hash(token_hash)
        return row or {}

    def client_by_token_hash(self, token_hash: str) -> Dict[str, Any] | None:
        row = self.connection.execute(
            """
            SELECT id, name, token_hash, token_prefix, notes, enabled, is_legacy,
                   created_at, last_seen, disabled_at
            FROM phone_clients WHERE token_hash = ?
            """,
            (str(token_hash),),
        ).fetchone()
        return dict(row) if row is not None else None

    def touch_client(self, client_id: int, *, timestamp: float | None = None) -> None:
        self.connection.execute(
            "UPDATE phone_clients SET last_seen = ? WHERE id = ?",
            (timestamp or time.time(), int(client_id)),
        )
        self.connection.commit()

    def clients(self) -> List[Dict[str, Any]]:
        now = time.time()
        rows = self.connection.execute(
            """
            SELECT id, name, token_prefix, notes, enabled, is_legacy,
                   created_at, last_seen, disabled_at
            FROM phone_clients ORDER BY is_legacy DESC, created_at DESC
            """
        ).fetchall()
        return [
            {
                **dict(row),
                "enabled": bool(row["enabled"]),
                "is_legacy": bool(row["is_legacy"]),
                "online": bool(row["enabled"] and row["last_seen"] and now - float(row["last_seen"]) <= CLIENT_ONLINE_SECONDS),
            }
            for row in rows
        ]

    def client_summary(self) -> Dict[str, int]:
        records = self.clients()
        return {
            "online": sum(1 for item in records if item["online"]),
            "registered": len(records),
            "enabled": sum(1 for item in records if item["enabled"]),
            "disabled": sum(1 for item in records if not item["enabled"]),
        }

    def create_client(self,
                      name: str,
                      token_hash: str,
                      token_prefix: str,
                      notes: str = "") -> Dict[str, Any]:
        now = time.time()
        cursor = self.connection.execute(
            """
            INSERT INTO phone_clients(
                name, token_hash, token_prefix, notes, enabled, is_legacy, created_at
            ) VALUES (?, ?, ?, ?, 1, 0, ?)
            """,
            (
                sanitize_summary(name, 48),
                str(token_hash),
                sanitize_summary(token_prefix, 16),
                sanitize_summary(notes, 160),
                now,
            ),
        )
        self.connection.commit()
        return next(item for item in self.clients() if item["id"] == int(cursor.lastrowid))

    def update_client(self,
                      client_id: int,
                      *,
                      name: str | None = None,
                      notes: str | None = None,
                      enabled: bool | None = None) -> Dict[str, Any] | None:
        current = self.connection.execute(
            "SELECT id FROM phone_clients WHERE id = ?",
            (int(client_id),),
        ).fetchone()
        if current is None:
            return None
        fields: List[str] = []
        values: List[Any] = []
        if name is not None:
            fields.append("name = ?")
            values.append(sanitize_summary(name, 48))
        if notes is not None:
            fields.append("notes = ?")
            values.append(sanitize_summary(notes, 160))
        if enabled is not None:
            fields.extend(["enabled = ?", "disabled_at = ?"])
            values.extend([1 if enabled else 0, None if enabled else time.time()])
        if fields:
            values.append(int(client_id))
            self.connection.execute(
                f"UPDATE phone_clients SET {', '.join(fields)} WHERE id = ?",
                values,
            )
            self.connection.commit()
        return next((item for item in self.clients() if item["id"] == int(client_id)), None)

    def replace_client_token(self,
                             client_id: int,
                             token_hash: str,
                             token_prefix: str) -> Dict[str, Any] | None:
        cursor = self.connection.execute(
            """
            UPDATE phone_clients SET token_hash = ?, token_prefix = ?, last_seen = NULL
            WHERE id = ? AND is_legacy = 0
            """,
            (str(token_hash), sanitize_summary(token_prefix, 16), int(client_id)),
        )
        self.connection.commit()
        if cursor.rowcount == 0:
            return None
        return next((item for item in self.clients() if item["id"] == int(client_id)), None)

    def add_log(self,
                level: str,
                module: str,
                message: str,
                *,
                timestamp: float | None = None) -> None:
        self.connection.execute(
            "INSERT INTO application_logs(ts, level, module, message) VALUES (?, ?, ?, ?)",
            (
                timestamp or time.time(),
                sanitize_summary(level.upper(), 12),
                sanitize_summary(module, 32),
                sanitize_summary(message, 400),
            ),
        )
        self.connection.commit()

    def logs(self,
             limit: int,
             *,
             level: str = "",
             module: str = "",
             query: str = "") -> List[Dict[str, Any]]:
        safe_limit = max(1, min(int(limit), 500))
        where: List[str] = []
        values: List[Any] = []
        if level:
            where.append("level = ?")
            values.append(level.upper()[:12])
        if module:
            where.append("module = ?")
            values.append(module[:32])
        if query:
            where.append("message LIKE ? ESCAPE '\\'")
            escaped = query.replace("\\", "\\\\").replace("%", "\\%").replace("_", "\\_")
            values.append(f"%{escaped}%")
        clause = f"WHERE {' AND '.join(where)}" if where else ""
        values.append(safe_limit)
        rows = self.connection.execute(
            f"SELECT id, ts, level, module, message FROM application_logs {clause} ORDER BY id DESC LIMIT ?",
            values,
        ).fetchall()
        return [dict(row) for row in rows]

    def power_settings(self) -> Dict[str, Any]:
        row = self.connection.execute(
            """
            SELECT absolute_limit_w, relative_multiplier, relative_step_w, updated_at
            FROM power_settings WHERE id = 1
            """
        ).fetchone()
        return dict(row) if row is not None else {
            "absolute_limit_w": DEFAULT_POWER_LIMIT_W,
            "relative_multiplier": 1.5,
            "relative_step_w": 10.0,
            "updated_at": time.time(),
        }

    def update_power_settings(self,
                              *,
                              absolute_limit_w: float,
                              relative_multiplier: float = 1.5,
                              relative_step_w: float = 10.0) -> Dict[str, Any]:
        limit = max(1.0, min(float(absolute_limit_w), 1000.0))
        multiplier = max(1.05, min(float(relative_multiplier), 5.0))
        step = max(0.1, min(float(relative_step_w), 1000.0))
        self.connection.execute(
            """
            UPDATE power_settings
            SET absolute_limit_w = ?, relative_multiplier = ?, relative_step_w = ?, updated_at = ?
            WHERE id = 1
            """,
            (limit, multiplier, step, time.time()),
        )
        self.connection.commit()
        return self.power_settings()

    def power_alerts(self, limit: int = 100, *, active_only: bool = False) -> List[Dict[str, Any]]:
        safe_limit = max(1, min(int(limit), 500))
        where = "WHERE status = 'active'" if active_only else ""
        rows = self.connection.execute(
            f"""
            SELECT id, started_at, updated_at, ended_at, status, power_w,
                   baseline_w, threshold_w, detail, acknowledged, acknowledged_at
            FROM power_alerts {where} ORDER BY id DESC LIMIT ?
            """,
            (safe_limit,),
        ).fetchall()
        return [
            {**dict(row), "acknowledged": bool(row["acknowledged"])}
            for row in rows
        ]

    def acknowledge_power_alert(self, alert_id: int) -> Dict[str, Any] | None:
        self.connection.execute(
            "UPDATE power_alerts SET acknowledged = 1, acknowledged_at = ? WHERE id = ?",
            (time.time(), int(alert_id)),
        )
        self.connection.commit()
        return next((item for item in self.power_alerts(500) if item["id"] == int(alert_id)), None)

    def evaluate_power(self,
                       power_total_w: float | None,
                       cool: bool,
                       *,
                       timestamp: float | None = None) -> Dict[str, Any] | None:
        now = timestamp or time.time()
        active = self.power_alerts(1, active_only=True)
        active_alert = active[0] if active else None
        if not cool or power_total_w is None:
            self.cool_started_at = None
            self.power_normal_streak = 0
            if active_alert is not None:
                self.connection.execute(
                    "UPDATE power_alerts SET status = 'recovered', ended_at = ?, updated_at = ? WHERE id = ?",
                    (now, now, active_alert["id"]),
                )
                self.connection.commit()
            return None

        power = max(0.0, float(power_total_w))
        if self.cool_started_at is None:
            self.cool_started_at = now
            return None
        if now - self.cool_started_at < POWER_STARTUP_GRACE_SECONDS:
            return None

        settings = self.power_settings()
        baseline = statistics.median(self.power_baseline) if len(self.power_baseline) >= POWER_BASELINE_MIN_SAMPLES else None
        absolute_hit = power > float(settings["absolute_limit_w"])
        adaptive_hit = bool(
            baseline is not None
            and power > baseline * float(settings["relative_multiplier"])
            and power - baseline >= float(settings["relative_step_w"])
        )
        if absolute_hit or adaptive_hit:
            self.power_normal_streak = 0
            threshold = float(settings["absolute_limit_w"])
            if adaptive_hit and baseline is not None:
                threshold = min(threshold, baseline * float(settings["relative_multiplier"]))
            reason = "超过管理员功率上限" if absolute_hit else "超过自适应制冷基线"
            detail = f"{reason}：当前{power:.1f}W，基线{baseline:.1f}W" if baseline is not None else f"{reason}：当前{power:.1f}W"
            if active_alert is not None:
                self.connection.execute(
                    """
                    UPDATE power_alerts SET updated_at = ?, power_w = MAX(power_w, ?),
                           baseline_w = ?, threshold_w = ?, detail = ? WHERE id = ?
                    """,
                    (now, power, baseline, threshold, detail, active_alert["id"]),
                )
                self.connection.commit()
                result = next(item for item in self.power_alerts(1, active_only=True))
                result["newly_created"] = False
                return result
            recent = self.connection.execute(
                """
                SELECT id FROM power_alerts
                WHERE status = 'recovered' AND updated_at >= ?
                ORDER BY id DESC LIMIT 1
                """,
                (now - POWER_ALERT_COOLDOWN_SECONDS,),
            ).fetchone()
            if recent is not None:
                self.connection.execute(
                    """
                    UPDATE power_alerts
                    SET status = 'active', ended_at = NULL, updated_at = ?,
                        power_w = MAX(power_w, ?), baseline_w = ?, threshold_w = ?, detail = ?
                    WHERE id = ?
                    """,
                    (now, power, baseline, threshold, detail, int(recent["id"])),
                )
                self.connection.commit()
                result = next(item for item in self.power_alerts(10) if item["id"] == int(recent["id"]))
                result["newly_created"] = False
                return result
            cursor = self.connection.execute(
                """
                INSERT INTO power_alerts(
                    started_at, updated_at, status, power_w, baseline_w, threshold_w, detail
                ) VALUES (?, ?, 'active', ?, ?, ?, ?)
                """,
                (now, now, power, baseline, threshold, detail),
            )
            self.connection.commit()
            result = next(item for item in self.power_alerts(10) if item["id"] == int(cursor.lastrowid))
            result["newly_created"] = True
            return result

        self.power_baseline.append(power)
        if active_alert is not None:
            self.power_normal_streak += 1
            if self.power_normal_streak >= 10:
                self.connection.execute(
                    "UPDATE power_alerts SET status = 'recovered', ended_at = ?, updated_at = ? WHERE id = ?",
                    (now, now, active_alert["id"]),
                )
                self.connection.commit()
                self.power_normal_streak = 0
        return None

    def create_firmware_release(self,
                                component: str,
                                version: str,
                                title: str,
                                changelog: str = "",
                                *,
                                channel: str = "stable",
                                published: bool = False,
                                is_current: bool = False) -> Dict[str, Any]:
        now = time.time()
        if is_current:
            self.connection.execute(
                "UPDATE firmware_releases SET is_current = 0 WHERE component = ?",
                (component,),
            )
        cursor = self.connection.execute(
            """
            INSERT INTO firmware_releases(
                component, version, title, changelog, channel, published,
                is_current, created_at, updated_at
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (
                component,
                sanitize_summary(version, 40),
                sanitize_summary(title, 100),
                sanitize_summary(changelog, 1000),
                channel,
                1 if published else 0,
                1 if is_current else 0,
                now,
                now,
            ),
        )
        self.connection.commit()
        return self.firmware_release(int(cursor.lastrowid)) or {}

    def firmware_release(self, release_id: int) -> Dict[str, Any] | None:
        rows = self.firmware_releases(include_unpublished=True)
        return next((item for item in rows if item["id"] == int(release_id)), None)

    def firmware_releases(self, *, include_unpublished: bool = False) -> List[Dict[str, Any]]:
        where = "" if include_unpublished else "WHERE published = 1"
        rows = self.connection.execute(
            f"""
            SELECT id, component, version, title, changelog, channel, published,
                   is_current, created_at, updated_at
            FROM firmware_releases {where}
            ORDER BY is_current DESC, created_at DESC
            """
        ).fetchall()
        result: List[Dict[str, Any]] = []
        for row in rows:
            item = {**dict(row), "published": bool(row["published"]), "is_current": bool(row["is_current"])}
            artifacts = self.connection.execute(
                """
                SELECT id, kind, filename, size_bytes, sha256, downloads, created_at,
                       target_part, image_version, min_bootloader, signature_type,
                       image_valid, validation_error
                FROM firmware_artifacts WHERE release_id = ? ORDER BY id
                """,
                (row["id"],),
            ).fetchall()
            item["artifacts"] = [
                {**dict(artifact), "image_valid": bool(artifact["image_valid"])}
                for artifact in artifacts
            ]
            result.append(item)
        return result

    def update_firmware_release(self,
                                release_id: int,
                                fields: Dict[str, Any]) -> Dict[str, Any] | None:
        current = self.firmware_release(release_id)
        if current is None:
            return None
        updates: List[str] = []
        values: List[Any] = []
        for key, limit in (("version", 40), ("title", 100), ("changelog", 1000), ("channel", 16)):
            if key in fields:
                updates.append(f"{key} = ?")
                values.append(sanitize_summary(str(fields[key]), limit))
        for key in ("published", "is_current"):
            if key in fields:
                if key == "is_current" and bool(fields[key]):
                    self.connection.execute(
                        "UPDATE firmware_releases SET is_current = 0 WHERE component = ?",
                        (current["component"],),
                    )
                updates.append(f"{key} = ?")
                values.append(1 if bool(fields[key]) else 0)
        if updates:
            updates.append("updated_at = ?")
            values.extend([time.time(), int(release_id)])
            self.connection.execute(
                f"UPDATE firmware_releases SET {', '.join(updates)} WHERE id = ?",
                values,
            )
            self.connection.commit()
        return self.firmware_release(release_id)

    def add_firmware_artifact(self,
                              release_id: int,
                              kind: str,
                              filename: str,
                              stored_name: str,
                              size_bytes: int,
                              sha256: str,
                              *,
                              target_part: str = "",
                              image_version: str = "",
                              min_bootloader: str = "",
                              signature_type: str = "",
                              image_valid: bool = False,
                              validation_error: str = "") -> Dict[str, Any]:
        cursor = self.connection.execute(
            """
            INSERT INTO firmware_artifacts(
                release_id, kind, filename, stored_name, size_bytes, sha256, created_at,
                target_part, image_version, min_bootloader, signature_type,
                image_valid, validation_error
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (
                int(release_id),
                kind,
                filename,
                stored_name,
                int(size_bytes),
                sha256,
                time.time(),
                sanitize_summary(target_part, 40),
                sanitize_summary(image_version, 40),
                sanitize_summary(min_bootloader, 40),
                sanitize_summary(signature_type, 32),
                1 if image_valid else 0,
                sanitize_summary(validation_error, 120),
            ),
        )
        self.connection.commit()
        return self.firmware_artifact(int(cursor.lastrowid)) or {}

    def firmware_artifact(self, artifact_id: int) -> Dict[str, Any] | None:
        row = self.connection.execute(
            """
            SELECT a.id, a.release_id, a.kind, a.filename, a.stored_name,
                   a.size_bytes, a.sha256, a.downloads, a.created_at,
                   a.target_part, a.image_version, a.min_bootloader,
                   a.signature_type, a.image_valid, a.validation_error,
                   r.component, r.version, r.published
            FROM firmware_artifacts a
            JOIN firmware_releases r ON r.id = a.release_id
            WHERE a.id = ?
            """,
            (int(artifact_id),),
        ).fetchone()
        if row is None:
            return None
        result = dict(row)
        result["image_valid"] = bool(result["image_valid"])
        return result

    def increment_firmware_download(self, artifact_id: int) -> None:
        self.connection.execute(
            "UPDATE firmware_artifacts SET downloads = downloads + 1 WHERE id = ?",
            (int(artifact_id),),
        )
        self.connection.commit()

    def delete_firmware_artifact(self, artifact_id: int) -> Dict[str, Any] | None:
        artifact = self.firmware_artifact(artifact_id)
        if artifact is None:
            return None
        active = self.connection.execute(
            """
            SELECT 1 FROM ota_deployments
            WHERE artifact_id = ?
              AND status IN ('queued', 'offered', 'transferring', 'rebooting', 'trial')
            LIMIT 1
            """,
            (int(artifact_id),),
        ).fetchone()
        if active is not None:
            raise ValueError("artifact_has_active_deployment")
        self.connection.execute("DELETE FROM firmware_artifacts WHERE id = ?", (int(artifact_id),))
        self.connection.commit()
        references = self.connection.execute(
            "SELECT COUNT(*) AS count FROM firmware_artifacts WHERE stored_name = ?",
            (artifact["stored_name"],),
        ).fetchone()
        artifact["remaining_references"] = int(references["count"] if references else 0)
        return artifact

    def delete_firmware_release(self, release_id: int) -> List[Dict[str, Any]] | None:
        release = self.firmware_release(release_id)
        if release is None:
            return None
        active = self.connection.execute(
            """
            SELECT 1
            FROM ota_deployments d
            JOIN firmware_artifacts a ON a.id = d.artifact_id
            WHERE a.release_id = ?
              AND d.status IN ('queued', 'offered', 'transferring', 'rebooting', 'trial')
            LIMIT 1
            """,
            (int(release_id),),
        ).fetchone()
        if active is not None:
            raise ValueError("release_has_active_deployment")
        artifacts = [self.firmware_artifact(item["id"]) for item in release.get("artifacts", [])]
        self.connection.execute("DELETE FROM firmware_releases WHERE id = ?", (int(release_id),))
        self.connection.commit()
        result: List[Dict[str, Any]] = []
        for artifact in artifacts:
            if artifact is None:
                continue
            references = self.connection.execute(
                "SELECT COUNT(*) AS count FROM firmware_artifacts WHERE stored_name = ?",
                (artifact["stored_name"],),
            ).fetchone()
            artifact["remaining_references"] = int(references["count"] if references else 0)
            result.append(artifact)
        return result

    def upsert_ota_device(self,
                          device_id: str,
                          part_number: str,
                          app_version: str,
                          boot_version: str,
                          active_bank: int,
                          *,
                          ota_state: str | None = None,
                          received_bytes: int | None = None,
                          total_bytes: int | None = None,
                          current_deployment_id: str | None = None,
                          last_result: str | None = None,
                          last_error: str | None = None,
                          timestamp: float | None = None) -> Dict[str, Any]:
        now = float(timestamp or time.time())
        existing = self.ota_device(device_id)
        values = {
            "device_id": sanitize_summary(device_id, 48),
            "part_number": sanitize_summary(part_number, 40),
            "app_version": sanitize_summary(app_version, 40),
            "boot_version": sanitize_summary(boot_version, 40),
            "active_bank": int(active_bank),
            "ota_state": sanitize_summary(
                ota_state if ota_state is not None else (existing or {}).get("ota_state", "idle"),
                24,
            ),
            "received_bytes": max(
                0,
                int(received_bytes if received_bytes is not None else (existing or {}).get("received_bytes", 0)),
            ),
            "total_bytes": max(
                0,
                int(total_bytes if total_bytes is not None else (existing or {}).get("total_bytes", 0)),
            ),
            "current_deployment_id": (
                sanitize_summary(current_deployment_id, 48)
                if current_deployment_id is not None
                else (existing or {}).get("current_deployment_id")
            ),
            "last_result": sanitize_summary(
                last_result if last_result is not None else (existing or {}).get("last_result", ""),
                32,
            ),
            "last_error": sanitize_summary(
                last_error if last_error is not None else (existing or {}).get("last_error", ""),
                120,
            ),
        }
        self.connection.execute(
            """
            INSERT INTO ota_devices(
                device_id, part_number, app_version, boot_version, active_bank,
                ota_state, received_bytes, total_bytes, current_deployment_id,
                last_result, last_error, last_seen, updated_at
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            ON CONFLICT(device_id) DO UPDATE SET
                part_number = excluded.part_number,
                app_version = excluded.app_version,
                boot_version = excluded.boot_version,
                active_bank = excluded.active_bank,
                ota_state = excluded.ota_state,
                received_bytes = excluded.received_bytes,
                total_bytes = excluded.total_bytes,
                current_deployment_id = excluded.current_deployment_id,
                last_result = excluded.last_result,
                last_error = excluded.last_error,
                last_seen = excluded.last_seen,
                updated_at = excluded.updated_at
            """,
            (
                values["device_id"],
                values["part_number"],
                values["app_version"],
                values["boot_version"],
                values["active_bank"],
                values["ota_state"],
                values["received_bytes"],
                values["total_bytes"],
                values["current_deployment_id"],
                values["last_result"],
                values["last_error"],
                now,
                now,
            ),
        )
        self.connection.commit()
        return self.ota_device(values["device_id"]) or {}

    def ota_device(self, device_id: str) -> Dict[str, Any] | None:
        row = self.connection.execute(
            "SELECT * FROM ota_devices WHERE device_id = ?",
            (str(device_id),),
        ).fetchone()
        return dict(row) if row is not None else None

    def ota_devices(self, *, online_seconds: float = 8.0) -> List[Dict[str, Any]]:
        now = time.time()
        rows = self.connection.execute(
            "SELECT * FROM ota_devices ORDER BY last_seen DESC"
        ).fetchall()
        return [
            {
                **dict(row),
                "online": (now - float(row["last_seen"])) <= float(online_seconds),
            }
            for row in rows
        ]

    def create_ota_deployment(self,
                              deployment_id: str,
                              device_id: str,
                              artifact_id: int,
                              from_version: str,
                              target_version: str,
                              bytes_total: int,
                              requested_by: str) -> Dict[str, Any]:
        active = self.active_ota_deployment(device_id)
        if active is not None:
            raise ValueError("device_has_active_deployment")
        now = time.time()
        self.connection.execute(
            """
            INSERT INTO ota_deployments(
                id, device_id, artifact_id, from_version, target_version,
                status, bytes_total, bytes_sent, error_code, requested_by,
                created_at, updated_at
            ) VALUES (?, ?, ?, ?, ?, 'queued', ?, 0, '', ?, ?, ?)
            """,
            (
                sanitize_summary(deployment_id, 48),
                sanitize_summary(device_id, 48),
                int(artifact_id),
                sanitize_summary(from_version, 40),
                sanitize_summary(target_version, 40),
                max(0, int(bytes_total)),
                sanitize_summary(requested_by, 80),
                now,
                now,
            ),
        )
        self.connection.commit()
        return self.ota_deployment(deployment_id) or {}

    def ota_deployment(self, deployment_id: str) -> Dict[str, Any] | None:
        row = self.connection.execute(
            """
            SELECT d.*, a.filename, a.sha256, a.target_part, a.signature_type,
                   a.image_valid, r.version AS release_version
            FROM ota_deployments d
            LEFT JOIN firmware_artifacts a ON a.id = d.artifact_id
            LEFT JOIN firmware_releases r ON r.id = a.release_id
            WHERE d.id = ?
            """,
            (str(deployment_id),),
        ).fetchone()
        if row is None:
            return None
        result = dict(row)
        result["image_valid"] = bool(result.get("image_valid"))
        return result

    def ota_deployments(self,
                        *,
                        device_id: str | None = None,
                        limit: int = 50) -> List[Dict[str, Any]]:
        safe_limit = max(1, min(int(limit), 200))
        if device_id:
            rows = self.connection.execute(
                """
                SELECT id FROM ota_deployments
                WHERE device_id = ? ORDER BY created_at DESC LIMIT ?
                """,
                (str(device_id), safe_limit),
            ).fetchall()
        else:
            rows = self.connection.execute(
                "SELECT id FROM ota_deployments ORDER BY created_at DESC LIMIT ?",
                (safe_limit,),
            ).fetchall()
        return [
            deployment
            for row in rows
            if (deployment := self.ota_deployment(str(row["id"]))) is not None
        ]

    def active_ota_deployment(self, device_id: str) -> Dict[str, Any] | None:
        row = self.connection.execute(
            """
            SELECT id FROM ota_deployments
            WHERE device_id = ?
              AND status IN ('queued', 'offered', 'transferring', 'rebooting', 'trial')
            ORDER BY created_at DESC LIMIT 1
            """,
            (str(device_id),),
        ).fetchone()
        return self.ota_deployment(str(row["id"])) if row is not None else None

    def update_ota_deployment(self,
                              deployment_id: str,
                              *,
                              status: str | None = None,
                              bytes_sent: int | None = None,
                              error_code: str | None = None) -> Dict[str, Any] | None:
        current = self.ota_deployment(deployment_id)
        if current is None:
            return None
        now = time.time()
        updates = ["updated_at = ?"]
        values: List[Any] = [now]
        if status is not None:
            safe_status = sanitize_summary(status, 24)
            updates.append("status = ?")
            values.append(safe_status)
            if safe_status in {"offered", "transferring"} and current.get("started_at") is None:
                updates.append("started_at = ?")
                values.append(now)
            if safe_status in {"succeeded", "failed", "rolled_back", "cancelled"}:
                updates.append("completed_at = ?")
                values.append(now)
        if bytes_sent is not None:
            updates.append("bytes_sent = ?")
            values.append(max(0, min(int(bytes_sent), int(current["bytes_total"]))))
        if error_code is not None:
            updates.append("error_code = ?")
            values.append(sanitize_summary(error_code, 80))
        values.append(str(deployment_id))
        self.connection.execute(
            f"UPDATE ota_deployments SET {', '.join(updates)} WHERE id = ?",
            values,
        )
        self.connection.commit()
        return self.ota_deployment(deployment_id)

    def cancel_ota_deployment(self, deployment_id: str) -> Dict[str, Any] | None:
        current = self.ota_deployment(deployment_id)
        if current is None:
            return None
        if current["status"] not in {"queued", "offered"}:
            raise ValueError("deployment_cannot_be_cancelled")
        return self.update_ota_deployment(deployment_id, status="cancelled")

    def record_metric(self,
                      channel: str,
                      operation: str,
                      success: bool,
                      latency_ms: float,
                      *,
                      timestamp: float | None = None) -> None:
        ts = timestamp or time.time()
        bucket = int(ts // 60) * 60
        latency = round(max(0.0, latency_ms), 2)
        self.connection.execute(
            """
            INSERT INTO metric_buckets(
                bucket_ts, channel, operation, request_count, success_count,
                total_latency_ms, max_latency_ms
            ) VALUES (?, ?, ?, 1, ?, ?, ?)
            ON CONFLICT(bucket_ts, channel, operation) DO UPDATE SET
                request_count = request_count + 1,
                success_count = success_count + excluded.success_count,
                total_latency_ms = total_latency_ms + excluded.total_latency_ms,
                max_latency_ms = MAX(max_latency_ms, excluded.max_latency_ms)
            """,
            (
                bucket,
                str(channel)[:24],
                str(operation)[:48],
                1 if success else 0,
                latency,
                latency,
            ),
        )
        self.connection.commit()

    def sample_status(self, status: Dict[str, Any]) -> None:
        now = time.time()
        online = bool(status.get("online"))

        if self.last_online is None or online != self.last_online:
            self.add_event(
                "device",
                "success" if online else "danger",
                "冰箱设备已上线" if online else "冰箱设备已离线",
                "W800心跳恢复" if online else "超过8秒未收到状态心跳",
                timestamp=now,
            )
            self.last_online = online

        if now - self.last_sample_at < TELEMETRY_SAMPLE_SECONDS:
            return
        self.last_sample_at = now
        self.connection.execute(
            """
            INSERT INTO telemetry(
                ts, online, set_temp, current_temp, fan, cool, door, vision_slot, vision_name,
                power_40_w, power_41_w, power_total_w, network_rtt_ms, heartbeat_gap_ms
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (
                now,
                1 if online else 0,
                float(status.get("set_temp") or 0.0),
                float(status.get("current_temp") or 0.0),
                int(status.get("fan") or 0),
                1 if status.get("cool") else 0,
                1 if status.get("door") else 0,
                status.get("vision_slot"),
                str(status.get("vision_name") or ""),
                status.get("power_40_w"),
                status.get("power_41_w"),
                status.get("power_total_w"),
                status.get("network_rtt_ms"),
                status.get("heartbeat_gap_ms"),
            ),
        )
        self.connection.commit()

        if now - self.last_cleanup_at >= 3600:
            self.cleanup(now)

    def cleanup(self, now: float | None = None) -> None:
        now = now or time.time()
        self.connection.execute("DELETE FROM telemetry WHERE ts < ?", (now - TELEMETRY_RETENTION_SECONDS,))
        self.connection.execute("DELETE FROM events WHERE ts < ?", (now - EVENT_RETENTION_SECONDS,))
        self.connection.execute("DELETE FROM health_records WHERE ts < ?", (now - HEALTH_RETENTION_SECONDS,))
        self.connection.execute("DELETE FROM metric_buckets WHERE bucket_ts < ?", (now - METRIC_RETENTION_SECONDS,))
        self.connection.execute("DELETE FROM admin_sessions WHERE expires_at <= ?", (now,))
        self.connection.execute("DELETE FROM control_audit WHERE ts < ?", (now - AUDIT_RETENTION_SECONDS,))
        self.connection.execute("DELETE FROM application_logs WHERE ts < ?", (now - LOG_RETENTION_SECONDS,))
        self.connection.execute(
            "DELETE FROM events WHERE id NOT IN (SELECT id FROM events ORDER BY id DESC LIMIT ?)",
            (MAX_EVENT_ROWS,),
        )
        self.connection.execute(
            "DELETE FROM health_records WHERE id NOT IN (SELECT id FROM health_records ORDER BY id DESC LIMIT ?)",
            (MAX_HEALTH_ROWS,),
        )
        self.connection.execute(
            "DELETE FROM control_audit WHERE id NOT IN (SELECT id FROM control_audit ORDER BY id DESC LIMIT ?)",
            (MAX_AUDIT_ROWS,),
        )
        self.connection.execute(
            "DELETE FROM application_logs WHERE id NOT IN (SELECT id FROM application_logs ORDER BY id DESC LIMIT ?)",
            (MAX_LOG_ROWS,),
        )
        self.connection.commit()
        self.last_cleanup_at = now

    @staticmethod
    def inventory(records: Iterable[Dict[str, Any]]) -> Dict[str, Any]:
        items: List[Dict[str, Any]] = []
        summary = {
            "capacity": 0, "occupied": 0, "empty": 0, "total_quantity": 0,
            "normal": 0, "expiring": 0, "expired": 0, "unknown": 0,
        }
        for source in records:
            summary["capacity"] += 1
            valid = bool(source.get("valid"))
            item = {
                "valid": valid,
                "slot": int(source.get("slot", 0)),
                "quantity": int(source.get("quantity", 1 if valid else 0)),
                "name": str(source.get("name", "")),
                "production_date": str(source.get("production_date", "")),
                "shelf_life": str(source.get("shelf_life", "")),
                "efficacy": str(source.get("efficacy", "")),
            }
            if not item["valid"]:
                item.update({"expiry_date": None, "expiry_status": "empty", "days_remaining": None})
                summary["empty"] += 1
            else:
                item.update(medicine_expiry(item["production_date"], item["shelf_life"]))
                summary["occupied"] += 1
                summary["total_quantity"] += item["quantity"]
                summary[item["expiry_status"]] += 1
            items.append(item)
        return {"summary": summary, "records": items}

    def history(self, range_key: str) -> Dict[str, Any]:
        ranges = {"1h": 3600, "6h": 6 * 3600, "24h": 24 * 3600}
        key = range_key if range_key in ranges else "1h"
        start = time.time() - ranges[key]
        rows = self.connection.execute(
            """
            SELECT ts, online, set_temp, current_temp, fan, cool, door,
                   power_40_w, power_41_w, power_total_w, network_rtt_ms, heartbeat_gap_ms
            FROM telemetry WHERE ts >= ? ORDER BY ts ASC
            """,
            (start,),
        ).fetchall()

        max_points = 720
        step = max(1, math.ceil(len(rows) / max_points))
        points: List[Dict[str, Any]] = []
        for offset in range(0, len(rows), step):
            chunk = rows[offset:offset + step]
            last = chunk[-1]
            points.append(
                {
                    "ts": float(last["ts"]),
                    "online": bool(last["online"]),
                    "set_temp": round(sum(float(row["set_temp"]) for row in chunk) / len(chunk), 2),
                    "current_temp": round(sum(float(row["current_temp"]) for row in chunk) / len(chunk), 2),
                    "fan": int(last["fan"]),
                    "cool": bool(last["cool"]),
                    "door": bool(last["door"]),
                    "power_40_w": _average_optional(chunk, "power_40_w"),
                    "power_41_w": _average_optional(chunk, "power_41_w"),
                    "power_total_w": _average_optional(chunk, "power_total_w"),
                    "network_rtt_ms": _average_optional(chunk, "network_rtt_ms"),
                    "heartbeat_gap_ms": _average_optional(chunk, "heartbeat_gap_ms"),
                }
            )
        return {"range": key, "sample_seconds": TELEMETRY_SAMPLE_SECONDS, "points": points}

    def events(self, limit: int) -> List[Dict[str, Any]]:
        safe_limit = max(1, min(int(limit), 100))
        rows = self.connection.execute(
            "SELECT ts, category, severity, title, detail FROM events ORDER BY id DESC LIMIT ?",
            (safe_limit,),
        ).fetchall()
        return [dict(row) for row in rows]

    def health(self, limit: int) -> List[Dict[str, Any]]:
        safe_limit = max(1, min(int(limit), 50))
        rows = self.connection.execute(
            """
            SELECT ts, symptom_summary, diagnosis, medicine, success, latency_ms
            FROM health_records ORDER BY id DESC LIMIT ?
            """,
            (safe_limit,),
        ).fetchall()
        return [
            {
                **dict(row),
                "success": bool(row["success"]),
            }
            for row in rows
        ]

    def _metric_summary(self) -> Dict[str, Any]:
        now_local = datetime.now().astimezone()
        today_start = now_local.replace(hour=0, minute=0, second=0, microsecond=0).timestamp()
        row = self.connection.execute(
            """
            SELECT
                COALESCE(SUM(request_count), 0) AS requests,
                COALESCE(SUM(success_count), 0) AS successes,
                COALESCE(SUM(total_latency_ms), 0) AS latency
            FROM metric_buckets
            WHERE bucket_ts >= ? AND channel != 'dashboard'
            """,
            (today_start,),
        ).fetchone()
        ai_row = self.connection.execute(
            """
            SELECT COALESCE(SUM(request_count), 0) AS requests
            FROM metric_buckets
            WHERE bucket_ts >= ? AND operation IN ('health_assess', 'medicine_recommend', 'chat')
            """,
            (today_start,),
        ).fetchone()
        requests = int(row["requests"])
        successes = int(row["successes"])
        return {
            "today_requests": requests,
            "today_ai_requests": int(ai_row["requests"]),
            "success_rate": round((successes / requests) * 100.0, 1) if requests else 100.0,
            "average_latency_ms": round(float(row["latency"]) / requests, 1) if requests else 0.0,
        }

    def _system_stats(self) -> Dict[str, Any]:
        result = {
            "metrics_available": psutil is not None,
            "cpu_percent": 0.0,
            "memory_percent": 0.0,
            "process_memory_mb": 0.0,
            "uptime_seconds": round(time.time() - self.started_at),
            "model": self.model,
            "tcp_port": self.tcp_port,
            "http_port": self.http_port,
        }
        if psutil is not None and self.process is not None:
            result.update(
                {
                    "cpu_percent": round(float(psutil.cpu_percent(interval=None)), 1),
                    "memory_percent": round(float(psutil.virtual_memory().percent), 1),
                    "process_memory_mb": round(self.process.memory_info().rss / (1024 * 1024), 1),
                }
            )
        return result

    def overview(self,
                 status: Dict[str, Any],
                 records: Iterable[Dict[str, Any]],
                 *,
                 control_pending: bool) -> Dict[str, Any]:
        inventory = self.inventory(records)
        metrics = self._metric_summary()
        system = self._system_stats()
        alerts: List[Dict[str, str]] = []
        online = bool(status.get("online"))
        if not online:
            alerts.append({"level": "danger", "title": "设备离线", "detail": "超过8秒未收到W800状态心跳"})
        else:
            current = float(status.get("current_temp") or 0.0)
            target = float(status.get("set_temp") or 0.0)
            if abs(current - target) > 3.0:
                alerts.append({"level": "warning", "title": "温差较大", "detail": f"当前温度与设定温度相差 {abs(current - target):.1f}℃"})
            if status.get("door"):
                alerts.append({"level": "warning", "title": "箱门开启", "detail": "检测到箱门处于开启状态"})
        if inventory["summary"]["expired"]:
            alerts.append({"level": "danger", "title": "存在过期药品", "detail": f"共 {inventory['summary']['expired']} 个槽位"})
        if inventory["summary"]["expiring"]:
            alerts.append({"level": "warning", "title": "存在临期药品", "detail": f"30天内到期 {inventory['summary']['expiring']} 个槽位"})
        if system["cpu_percent"] >= 85.0 or system["memory_percent"] >= 85.0:
            alerts.append({"level": "warning", "title": "服务器负载较高", "detail": "CPU或内存使用率超过85%"})

        latest_health = self.health(1)
        public_status = {
            "online": online,
            "last_seen": float(status.get("last_seen") or 0.0),
            "last_seen_age_seconds": status.get("last_seen_age_seconds"),
            "set_temp": status.get("set_temp"),
            "current_temp": status.get("current_temp"),
            "fan": status.get("fan"),
            "cool": bool(status.get("cool")),
            "door": bool(status.get("door")),
            "water_cool": bool(status.get("water_cool")),
            "vision_slot": status.get("vision_slot"),
            "vision_code": status.get("vision_code", status.get("vision_slot")),
            "vision_name": status.get("vision_name", ""),
            "vision_updated_at": status.get("vision_updated_at", 0.0),
            "power_40_w": status.get("power_40_w"),
            "power_41_w": status.get("power_41_w"),
            "power_total_w": status.get("power_total_w"),
            "network_rtt_ms": status.get("network_rtt_ms"),
            "heartbeat_gap_ms": status.get("heartbeat_gap_ms"),
            "control_pending": bool(control_pending),
        }
        return {
            "generated_at": time.time(),
            "public_demo": True,
            "device": public_status,
            "inventory": inventory["summary"],
            "metrics": metrics,
            "server": system,
            "alerts": alerts,
            "power_alerts": self.power_alerts(5, active_only=True),
            "power_settings": self.power_settings(),
            "clients": self.client_summary(),
            "latest_health": latest_health[0] if latest_health else None,
        }
