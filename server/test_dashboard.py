from __future__ import annotations

import asyncio
import json
import math
import sys
import tempfile
import time
import types
import unittest
from unittest.mock import AsyncMock, patch
from datetime import date
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

# The local test machine may not have the production OpenAI-compatible SDK.
# Dashboard and routing tests do not call DeepSeek, so a tiny import stub keeps
# these tests offline while requirements.txt still installs the real SDK online.
if "openai" not in sys.modules:
    openai_stub = types.ModuleType("openai")

    class AsyncOpenAI:  # pragma: no cover - only needed to import the server module.
        def __init__(self, *args, **kwargs):
            pass

    openai_stub.AsyncOpenAI = AsyncOpenAI
    sys.modules["openai"] = openai_stub

from dashboard import DashboardStore, medicine_expiry, normalize_production_date, sanitize_summary
import fridge_deepseek_server as server


class DashboardDataTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tempdir = tempfile.TemporaryDirectory()
        self.db_path = Path(self.tempdir.name) / "dashboard.db"
        self.store = DashboardStore(self.db_path, model="test-model", tcp_port=10086, http_port=10087)

    def tearDown(self) -> None:
        self.store.close()
        self.tempdir.cleanup()

    def test_health_summary_is_short_and_single_line(self) -> None:
        raw = "头部症状：头疼\r\n  四肢症状：关节僵硬 " + "很长" * 50
        result = sanitize_summary(raw)
        self.assertNotIn("\n", result)
        self.assertNotIn("\r", result)
        self.assertLessEqual(len(result), 60)

    def test_expiry_supports_days_months_and_years(self) -> None:
        today = date(2026, 7, 15)
        self.assertEqual(medicine_expiry("2026-07-01", "30天", today=today)["expiry_status"], "expiring")
        self.assertEqual(medicine_expiry("2026-01-31", "6个月", today=today)["expiry_date"], "2026-07-31")
        self.assertEqual(medicine_expiry("2024-01-01", "1年", today=today)["expiry_status"], "expired")
        self.assertEqual(medicine_expiry("未知", "12个月", today=today)["expiry_status"], "unknown")

    def test_production_date_accepts_all_supported_formats(self) -> None:
        for raw in ("2025-07-16", "2025/7/16", "2025.7.16", "2025年7月16日"):
            self.assertEqual(normalize_production_date(raw), "2025-07-16")
            result = medicine_expiry(raw, "12个月", today=date(2026, 7, 1))
            self.assertEqual(result["expiry_date"], "2026-07-16")
            self.assertEqual(result["expiry_status"], "expiring")
        with self.assertRaises(ValueError):
            normalize_production_date("2025年2月30日")

    def test_persistence_history_inventory_and_metrics(self) -> None:
        status = {
            "online": True,
            "set_temp": -3,
            "current_temp": -4.6,
            "fan": 2,
            "cool": True,
            "door": False,
            "vision_slot": 2,
            "vision_name": "感冒灵颗粒",
            "vision_updated_at": time.time(),
            "power_40_w": 18.2,
            "power_41_w": 21.4,
            "power_total_w": 39.6,
            "network_rtt_ms": 35.0,
            "heartbeat_gap_ms": 1001.0,
        }
        self.store.sample_status(status)
        self.store.add_health("发热\n头疼", "发烧", "感冒灵颗粒", True, 238.5)
        self.store.record_metric("w800", "health_assess", True, 238.5)
        inventory = self.store.inventory([
            {"valid": True, "slot": 0, "quantity": 7, "name": "药A", "production_date": "2026-07-01", "shelf_life": "30天", "efficacy": ""},
            {"valid": False, "slot": 1, "name": "", "production_date": "", "shelf_life": "", "efficacy": ""},
        ])
        self.assertEqual(inventory["summary"]["capacity"], 2)
        self.assertEqual(inventory["summary"]["occupied"], 1)
        self.assertEqual(inventory["summary"]["total_quantity"], 7)
        self.assertEqual(self.store.history("1h")["points"][0]["current_temp"], -4.6)
        self.assertEqual(self.store.history("1h")["points"][0]["power_total_w"], 39.6)
        self.assertEqual(self.store.history("1h")["points"][0]["network_rtt_ms"], 35.0)
        self.store.close()

        reopened = DashboardStore(self.db_path, model="test-model", tcp_port=10086, http_port=10087)
        self.store = reopened
        self.assertEqual(reopened.health(10)[0]["symptom_summary"], "发热 头疼")
        self.assertTrue(reopened.events(10))
        self.assertEqual(reopened._metric_summary()["today_ai_requests"], 1)

    def test_zero_quantity_is_not_accepted_as_ai_inventory(self) -> None:
        records = [
            {"valid": True, "slot": 0, "quantity": 0, "name": "感冒灵颗粒", "efficacy": "感冒"},
        ]
        self.assertEqual(server.select_inventory_medicine("感冒灵颗粒", records), "无对应药品")

    def test_admin_sessions_and_audit_are_persistent(self) -> None:
        now = time.time()
        self.store.create_admin_session("hash-a", "csrf-a", now + 3600, created_at=now)
        session = self.store.admin_session("hash-a", now=now + 1, touch=False)
        self.assertIsNotNone(session)
        self.assertEqual(session["csrf_token"], "csrf-a")

        self.store.add_audit(
            "web_admin", "device", "fridge_control",
            target="fan", summary="fan=2", result="pending", command_id="cmd-a",
        )
        self.store.update_audit_command("cmd-a", "confirmed", confirmed_at=now + 2)
        record = self.store.audit(1)[0]
        self.assertEqual(record["result"], "confirmed")
        self.assertEqual(record["command_id"], "cmd-a")

    def test_recent_vision_recognitions_are_unique_ordered_and_persistent(self) -> None:
        self.assertTrue(self.store.record_vision_recognition(0x00, "999感冒灵", timestamp=100.0))
        self.assertFalse(self.store.record_vision_recognition(0x00, "999感冒灵", timestamp=101.0))
        self.assertTrue(self.store.record_vision_recognition(0x06, "晚A茶", timestamp=102.0))
        self.assertTrue(self.store.record_vision_recognition(0x03, "头孢克肟胶囊", timestamp=103.0))
        self.assertTrue(self.store.record_vision_recognition(0x00, "999感冒灵", timestamp=104.0))
        records = self.store.recent_vision_recognitions(5)
        self.assertEqual([record["code"] for record in records], [0x00, 0x03, 0x06])
        self.assertEqual(records[0]["last_seen"], 104.0)

        self.assertTrue(self.store.record_vision_recognition(
            0x0E, "未识别到库存中药品", include_in_recent=False, timestamp=105.0
        ))
        self.assertEqual([record["code"] for record in self.store.recent_vision_recognitions(5)], [0x00, 0x03, 0x06])
        self.store.close()

        reopened = DashboardStore(self.db_path, model="test-model", tcp_port=10086, http_port=10087)
        self.store = reopened
        self.assertEqual(reopened.last_vision_code, 0x0E)
        self.assertFalse(reopened.record_vision_recognition(
            0x0E, "未识别到库存中药品", include_in_recent=False, timestamp=106.0
        ))
        self.assertEqual([record["code"] for record in reopened.recent_vision_recognitions(5)], [0x00, 0x03, 0x06])

    def test_phone_clients_power_alerts_and_firmware_catalog(self) -> None:
        client = self.store.create_client("答辩手机", "hash-1", "fc_demo…", "演示")
        self.store.touch_client(client["id"])
        self.assertEqual(self.store.client_by_token_hash("hash-1")["name"], "答辩手机")
        self.assertEqual(self.store.client_summary()["online"], 1)
        self.store.update_client(client["id"], enabled=False)
        self.assertEqual(self.store.client_summary()["disabled"], 1)

        self.store.evaluate_power(50.0, True, timestamp=100.0)
        for index in range(20):
            self.store.evaluate_power(50.0, True, timestamp=111.0 + index)
        alert = self.store.evaluate_power(90.0, True, timestamp=140.0)
        self.assertIsNotNone(alert)
        self.assertTrue(alert["newly_created"])
        self.assertEqual(self.store.power_alerts(1)[0]["status"], "active")
        for index in range(10):
            self.store.evaluate_power(50.0, True, timestamp=141.0 + index)
        self.assertEqual(self.store.power_alerts(1)[0]["status"], "recovered")

        release = self.store.create_firmware_release(
            "main", "v1.0.0", "主控稳定版", "初始版本", published=True, is_current=True,
        )
        artifact = self.store.add_firmware_artifact(
            release["id"], "install", "project.srec", "abc.srec", 123, "a" * 64,
        )
        public = self.store.firmware_releases()
        self.assertEqual(public[0]["artifacts"][0]["sha256"], "a" * 64)
        self.store.increment_firmware_download(artifact["id"])
        self.assertEqual(self.store.firmware_artifact(artifact["id"])["downloads"], 1)


class DashboardApiTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self) -> None:
        self.tempdir = tempfile.TemporaryDirectory()
        root = Path(self.tempdir.name)
        self.original_db_path = server.MEDICINE_DB_PATH
        self.original_store = server.dashboard_store
        self.original_admin_hash = server.ADMIN_PASSWORD_HASH
        self.original_allow_insecure = server.ALLOW_INSECURE_ADMIN
        self.original_firmware_root = server.FIRMWARE_ROOT
        server.MEDICINE_DB_PATH = root / "medicine_store.json"
        server.save_medicine_records(server.make_default_medicine_records())
        server.dashboard_store = DashboardStore(root / "dashboard.db", model="test", tcp_port=10086, http_port=10087)
        server.FIRMWARE_ROOT = root / "firmware"
        server.pending_control_command = None
        server.pending_door_command = None
        server.last_control_command = None
        server.admin_login_failures.clear()
        server.admin_login_locked_until.clear()

    async def asyncTearDown(self) -> None:
        if server.dashboard_store is not None:
            server.dashboard_store.close()
        server.dashboard_store = self.original_store
        server.MEDICINE_DB_PATH = self.original_db_path
        server.ADMIN_PASSWORD_HASH = self.original_admin_hash
        server.ALLOW_INSECURE_ADMIN = self.original_allow_insecure
        server.FIRMWARE_ROOT = self.original_firmware_root
        self.tempdir.cleanup()

    async def admin_login(self) -> tuple[dict, dict]:
        server.ALLOW_INSECURE_ADMIN = True
        server.ADMIN_PASSWORD_HASH = server.make_admin_password_hash(
            "correct-password", salt=b"0123456789abcdef"
        )
        response_headers: dict[str, str] = {}
        status, payload = await server.handle_http_api(
            "POST", "/api/admin/login", {"host": "localhost:10087"},
            b'{"password":"correct-password"}',
            peer_ip="127.0.0.1", response_headers=response_headers,
        )
        self.assertEqual(status, 200)
        cookie = response_headers["Set-Cookie"].split(";", 1)[0]
        headers = {
            "cookie": cookie,
            "host": "localhost:10087",
            "origin": "http://localhost:10087",
            "x-csrf-token": payload["csrf_token"],
        }
        return payload, headers

    async def test_admin_login_session_csrf_and_logout(self) -> None:
        login, headers = await self.admin_login()
        self.assertTrue(login["authenticated"])

        status, session = await server.handle_http_api(
            "GET", "/api/admin/session", {"cookie": headers["cookie"]}, b""
        )
        self.assertEqual(status, 200)
        self.assertEqual(session["csrf_token"], login["csrf_token"])

        status, bootstrap = await server.handle_http_api(
            "GET", "/api/admin/bootstrap", {"cookie": headers["cookie"]}, b""
        )
        self.assertEqual(status, 200)
        self.assertIn("capabilities", bootstrap)

        bad_headers = {"cookie": headers["cookie"], "host": headers["host"], "origin": headers["origin"]}
        status, _ = await server.handle_http_api(
            "PUT", "/api/medicines/0", bad_headers, b"{}"
        )
        self.assertEqual(status, 403)

        response_headers: dict[str, str] = {}
        status, _ = await server.handle_http_api(
            "POST", "/api/admin/logout", headers, b"", response_headers=response_headers
        )
        self.assertEqual(status, 200)
        self.assertIn("Max-Age=0", response_headers["Set-Cookie"])

        status, _ = await server.handle_http_api(
            "GET", "/api/admin/session", {"cookie": headers["cookie"]}, b""
        )
        self.assertEqual(status, 401)

    async def test_admin_login_rate_limit(self) -> None:
        server.ALLOW_INSECURE_ADMIN = True
        server.ADMIN_PASSWORD_HASH = server.make_admin_password_hash(
            "correct-password", salt=b"0123456789abcdef"
        )
        for _ in range(server.ADMIN_LOGIN_MAX_FAILURES):
            status, _ = await server.handle_http_api(
                "POST", "/api/admin/login", {}, b'{"password":"wrong-password"}', peer_ip="198.51.100.1"
            )
            self.assertEqual(status, 401)
        status, payload = await server.handle_http_api(
            "POST", "/api/admin/login", {}, b'{"password":"correct-password"}', peer_ip="198.51.100.1"
        )
        self.assertEqual(status, 429)
        self.assertGreater(payload["retry_after"], 0)

    async def test_web_control_requires_online_device_and_tracks_confirmation(self) -> None:
        _, headers = await self.admin_login()
        server.fridge_state["last_seen"] = 0.0
        status, _ = await server.handle_http_api(
            "POST", "/api/fridge/control", headers, b'{"fan":2}'
        )
        self.assertEqual(status, 409)

        server.fridge_control_initialized = True
        server.fridge_control_dirty = False
        server.fridge_state.update({
            "last_seen": time.time(), "set_temp": -3, "fan": 0,
            "cool": False, "water_cool": False,
        })
        server.fridge_control.update({
            "set_temp": -3, "fan": 0, "cool": False,
            "water_cool": False, "door": False,
        })
        status, payload = await server.handle_http_api(
            "POST", "/api/fridge/control", headers, b'{"fan":2}'
        )
        self.assertEqual(status, 200)
        self.assertEqual(payload["command"]["status"], "pending")
        command_id = payload["command_id"]

        await server.handle_fridge_status_command("FRIDGE STATUS -3|-30|2|0|0|255|0")
        self.assertFalse(server.fridge_control_dirty)
        self.assertEqual(server.last_control_command["status"], "confirmed")
        self.assertEqual(server.dashboard_store.audit(1)[0]["command_id"], command_id)
        self.assertEqual(server.dashboard_store.audit(1)[0]["result"], "confirmed")

    async def test_web_command_timeout_supersede_and_door_delivery(self) -> None:
        _, headers = await self.admin_login()
        server.fridge_control_initialized = True
        server.fridge_control_dirty = False
        server.fridge_state.update({
            "last_seen": time.time(), "set_temp": -3, "fan": 0,
            "cool": False, "water_cool": False,
        })
        server.fridge_control.update({
            "set_temp": -3, "fan": 0, "cool": False,
            "water_cool": False, "door": False,
        })

        status, first = await server.handle_http_api(
            "POST", "/api/fridge/control", headers, b'{"fan":1}'
        )
        self.assertEqual(status, 200)
        server.pending_control_command["requested_at"] = time.time() - 9
        server.refresh_control_commands_unlocked()
        self.assertEqual(server.pending_control_command["status"], "timeout")

        status, second = await server.handle_http_api(
            "POST", "/api/fridge/control", headers, b'{"fan":2}'
        )
        self.assertEqual(status, 200)
        self.assertNotEqual(first["command_id"], second["command_id"])
        audit = server.dashboard_store.audit(10)
        first_record = next(item for item in audit if item["command_id"] == first["command_id"])
        self.assertEqual(first_record["result"], "superseded")

        status, door = await server.handle_http_api(
            "POST", "/api/fridge/control", headers, b'{"door":true}'
        )
        self.assertEqual(status, 200)
        self.assertEqual(door["command"]["door_status"], "queued")
        reply = await server.handle_fridge_status_command(
            "FRIDGE STATUS -3|-30|0|0|0|255|0"
        )
        self.assertEqual(reply.removeprefix("[FRIDGE] OK ").split("|")[3], "1")
        door_record = next(
            item for item in server.dashboard_store.audit(10)
            if item["command_id"] == door["command_id"]
        )
        self.assertEqual(door_record["result"], "sent")

    async def test_public_get_needs_no_token_but_management_stays_protected(self) -> None:
        status, payload = await server.handle_http_api(
            "GET", "/api/public/dashboard/overview", {}, b""
        )
        self.assertEqual(status, 200)
        self.assertTrue(payload["public_demo"])

        status, _ = await server.handle_http_api("POST", "/api/public/dashboard/overview", {}, b"")
        self.assertEqual(status, 405)

        status, _ = await server.handle_http_api("GET", "/api/fridge/status", {}, b"")
        self.assertEqual(status, 401)

        status, payload = await server.handle_http_api(
            "GET", "/api/ping", {"x-fridge-token": server.APP_TOKEN}, b""
        )
        self.assertEqual(status, 200)
        self.assertEqual(payload["message"], "pong")

    async def test_repeated_status_does_not_create_a_new_vision_timestamp(self) -> None:
        server.fridge_state.update({
            "vision_slot": None, "vision_code": None,
            "vision_name": "", "vision_updated_at": 0.0,
        })
        command = "FRIDGE STATUS -3|-27|2|1|0|2"
        first_reply = await server.handle_fridge_status_command(command)
        first_timestamp = server.fridge_state["vision_updated_at"]
        second_reply = await server.handle_fridge_status_command(command)
        self.assertTrue(first_reply.startswith("[FRIDGE] OK"))
        self.assertTrue(second_reply.startswith("[FRIDGE] OK"))
        self.assertGreater(first_timestamp, 0.0)
        self.assertEqual(server.fridge_state["vision_updated_at"], first_timestamp)
        self.assertEqual(server.fridge_state["vision_name"], "地奎氨铵含片")

    async def test_admin_recent_recognitions_use_fixed_visual_mapping(self) -> None:
        _, headers = await self.admin_login()
        server.fridge_state.update({
            "vision_slot": None, "vision_code": None,
            "vision_name": "", "vision_updated_at": 0.0,
        })
        server.dashboard_store.last_vision_code = None

        for code in (0x00, 0x06, 0x03, 0x00):
            reply = await server.handle_fridge_status_command(
                f"FRIDGE STATUS -3|-27|2|1|0|{code}"
            )
            self.assertTrue(reply.startswith("[FRIDGE] OK"))

        status, payload = await server.handle_http_api(
            "GET", "/api/admin/recognitions?limit=5", {"cookie": headers["cookie"]}, b""
        )
        self.assertEqual(status, 200)
        self.assertEqual(
            [record["name"] for record in payload["records"]],
            ["999感冒灵", "头孢克肟胶囊", "晚A茶"],
        )

        before_unknown = list(payload["records"])
        await server.handle_fridge_status_command("FRIDGE STATUS -3|-27|2|1|0|14")
        status, payload = await server.handle_http_api(
            "GET", "/api/admin/recognitions?limit=5", {"cookie": headers["cookie"]}, b""
        )
        self.assertEqual(payload["records"], before_unknown)
        self.assertEqual(server.fridge_state["vision_name"], "未识别到库存中药品")

        status, latest = await server.handle_http_api(
            "GET", "/api/vision/latest", {"x-fridge-token": server.APP_TOKEN}, b""
        )
        self.assertEqual(status, 200)
        self.assertEqual(latest["vision"]["code"], 0x0E)
        self.assertEqual(latest["vision"]["slot"], 0x0E)

        status, _ = await server.handle_http_api(
            "GET", "/api/admin/recognitions?limit=5", {}, b""
        )
        self.assertEqual(status, 401)

    def test_all_visual_class_names_match_deployed_ra6m5_mapping(self) -> None:
        expected = [
            "999感冒灵", "云南白药气雾剂", "地奎氨铵含片", "头孢克肟胶囊",
            "头孢地尼胶囊", "对乙酰氨基酚缓释片", "晚A茶", "枸橼酸莫沙必利片",
            "氨麻苯美片", "磷酸奥司他韦胶囊", "肠炎宁片", "莲花清瘟胶囊",
            "蒙脱石散", "诺氟沙星胶囊", "未识别到库存中药品",
        ]
        self.assertEqual([server.vision_name_for_code(code) for code in range(15)], expected)

    async def test_water_cool_protocol_accepts_old_and_new_status_formats(self) -> None:
        server.fridge_control_initialized = False
        server.fridge_control_dirty = False
        server.fridge_state["water_cool"] = False
        server.fridge_control["water_cool"] = False

        old_reply = await server.handle_fridge_status_command(
            "FRIDGE STATUS -3|-27|2|1|0|2"
        )
        self.assertTrue(old_reply.startswith("[FRIDGE] OK"))
        self.assertEqual(len(old_reply.removeprefix("[FRIDGE] OK ").split("|")), 5)
        self.assertFalse(server.fridge_state["water_cool"])

        new_reply = await server.handle_fridge_status_command(
            "FRIDGE STATUS -3|-27|2|1|0|2|1"
        )
        self.assertTrue(new_reply.endswith("|1"))
        self.assertTrue(server.fridge_state["water_cool"])

    async def test_extended_status_parses_power_rtt_and_heartbeat(self) -> None:
        server.fridge_control_initialized = False
        server.fridge_control_dirty = False
        server.fridge_state["last_seen"] = time.time() - 1.25
        reply = await server.handle_fridge_status_command(
            "FRIDGE STATUS -3|-27|2|1|0|14|1|182|214|37"
        )
        self.assertTrue(reply.startswith("[FRIDGE] OK"))
        self.assertEqual(server.fridge_state["power_40_w"], 18.2)
        self.assertEqual(server.fridge_state["power_41_w"], 21.4)
        self.assertEqual(server.fridge_state["power_total_w"], 39.6)
        self.assertEqual(server.fridge_state["network_rtt_ms"], 37.0)
        self.assertGreater(server.fridge_state["heartbeat_gap_ms"], 1000.0)

        await server.handle_fridge_status_command(
            "FRIDGE STATUS -3|-27|2|1|0|14|1|NA|NA|NA"
        )
        self.assertIsNone(server.fridge_state["power_total_w"])
        self.assertIsNone(server.fridge_state["network_rtt_ms"])

    async def test_individual_phone_token_can_be_disabled(self) -> None:
        _, admin_headers = await self.admin_login()
        status, created = await server.handle_http_api(
            "POST", "/api/admin/clients", admin_headers,
            json.dumps({"name": "测试手机", "notes": "自动测试"}, ensure_ascii=False).encode("utf-8"),
        )
        self.assertEqual(status, 201)
        token = created["token"]
        client_id = created["client"]["id"]

        status, _ = await server.handle_http_api(
            "GET", "/api/ping", {"x-fridge-token": token}, b""
        )
        self.assertEqual(status, 200)
        clients = server.dashboard_store.clients()
        self.assertIsNotNone(next(item for item in clients if item["id"] == client_id)["last_seen"])

        status, _ = await server.handle_http_api(
            "PATCH", f"/api/admin/clients/{client_id}", admin_headers, b'{"enabled":false}'
        )
        self.assertEqual(status, 200)
        status, payload = await server.handle_http_api(
            "GET", "/api/ping", {"x-fridge-token": token}, b""
        )
        self.assertEqual(status, 403)
        self.assertEqual(payload["error"], "client_disabled")

    async def test_admin_firmware_stream_upload_and_public_catalog(self) -> None:
        _, admin_headers = await self.admin_login()
        status, created = await server.handle_http_api(
            "POST", "/api/admin/firmware/releases", admin_headers,
            json.dumps({
                "component": "vision", "version": "v-test", "title": "视觉测试模型",
                "channel": "beta", "published": True, "is_current": True,
            }, ensure_ascii=False).encode("utf-8"),
        )
        self.assertEqual(status, 201)
        release_id = created["release"]["id"]
        content = b"firmware-test-data"
        reader = asyncio.StreamReader()
        reader.feed_data(content)
        reader.feed_eof()
        upload_headers = {
            **admin_headers,
            "content-type": "application/octet-stream",
        }
        status, uploaded = await server.receive_firmware_upload(
            reader,
            f"/api/admin/firmware/artifacts?release_id={release_id}&kind=model&filename=model.eim",
            upload_headers,
            len(content),
        )
        self.assertEqual(status, 201)
        artifact = uploaded["artifact"]
        self.assertTrue((server.FIRMWARE_ROOT / artifact["stored_name"]).is_file())

        status, catalog = await server.handle_http_api(
            "GET", "/api/public/firmware/releases", {}, b""
        )
        self.assertEqual(status, 200)
        self.assertEqual(catalog["releases"][0]["component"], "vision")
        self.assertNotIn("stored_name", catalog["releases"][0]["artifacts"][0])

    async def test_http_medicine_date_is_normalized(self) -> None:
        headers = {"x-fridge-token": server.APP_TOKEN}
        body = json.dumps({
            "name": "日期测试药", "quantity": 1,
            "production_date": "2025年7月16日", "shelf_life": "12个月", "efficacy": "测试",
        }, ensure_ascii=False).encode("utf-8")
        status, payload = await server.handle_http_api("POST", "/api/medicines", headers, body)
        self.assertEqual(status, 201)
        self.assertEqual(payload["record"]["production_date"], "2025-07-16")

        invalid = json.dumps({
            "name": "日期测试药", "quantity": 1,
            "production_date": "2025年2月30日", "shelf_life": "12个月", "efficacy": "测试",
        }, ensure_ascii=False).encode("utf-8")
        status, payload = await server.handle_http_api("POST", "/api/medicines", headers, invalid)
        self.assertEqual(status, 400)
        self.assertIn("生产日期", payload["error"])

    async def test_http_water_cool_control_is_pending_until_device_confirms(self) -> None:
        server.fridge_control_initialized = True
        server.fridge_control_dirty = False
        server.fridge_state.update({
            "last_seen": time.time(), "set_temp": -3, "fan": 2,
            "cool": True, "water_cool": False,
        })
        server.fridge_control.update({
            "set_temp": -3, "fan": 2, "cool": True,
            "door": False, "water_cool": False,
        })

        status, payload = await server.handle_http_api(
            "POST",
            "/api/fridge/control",
            {"x-fridge-token": server.APP_TOKEN},
            b'{"water_cool":true}',
        )
        self.assertEqual(status, 200)
        self.assertTrue(payload["control"]["water_cool"])
        self.assertTrue(payload["control_pending"])

        reply = await server.handle_fridge_status_command(
            "FRIDGE STATUS -3|-27|2|1|0|2|1"
        )
        self.assertTrue(reply.endswith("|1"))
        self.assertFalse(server.fridge_control_dirty)

        # The old Android screen still sends door=false for its "close" button.
        status, payload = await server.handle_http_api(
            "POST", "/api/fridge/control",
            {"x-fridge-token": server.APP_TOKEN}, b'{"door":false}',
        )
        self.assertEqual(status, 200)
        self.assertTrue(payload["ok"])

    async def test_old_medicine_json_is_migrated_with_default_quantity(self) -> None:
        old_records = [server.make_blank_medicine_record(slot) for slot in range(32)]
        old_records[0] = {
            "valid": True, "slot": 0, "name": "旧药品",
            "production_date": "2025年7月16日", "shelf_life": "1年", "efficacy": "测试",
        }
        old_records[0].pop("quantity", None)
        server.MEDICINE_DB_PATH.write_text(
            json.dumps({"records": old_records}, ensure_ascii=False), encoding="utf-8"
        )

        migrated = server.load_medicine_records()
        persisted = json.loads(server.MEDICINE_DB_PATH.read_text(encoding="utf-8"))["records"]
        self.assertEqual(migrated[0]["quantity"], 1)
        self.assertEqual(migrated[0]["production_date"], "2025-07-16")
        self.assertEqual(migrated[1]["quantity"], 0)
        self.assertEqual(persisted[0]["quantity"], 1)
        self.assertEqual(persisted[0]["production_date"], "2025-07-16")

    async def test_legacy_and_quantity_medicine_protocols(self) -> None:
        add_reply = await server.handle_medicine_command(
            "MED ADD 测试药|2026-01-01|12个月|测试药效", None
        )
        self.assertTrue(add_reply.startswith("[MED] OK ADD "))
        slot = int(add_reply.removeprefix("[MED] OK ADD "))

        legacy_get = await server.handle_medicine_command(f"MED GET {slot}", None)
        getq = await server.handle_medicine_command(f"MED GETQ {slot}", None)
        self.assertEqual(len(legacy_get.removeprefix("[MED] OK GET ").split("|")), 6)
        self.assertEqual(len(getq.removeprefix("[MED] OK GETQ ").split("|")), 7)
        self.assertEqual(getq.removeprefix("[MED] OK GETQ ").split("|")[2], "1")

        await server.handle_medicine_command(f"MED RESTOCK {slot}|4", None)
        legacy_set = await server.handle_medicine_command(
            f"MED SET {slot}|测试药2|2026-02-01|18个月|新药效", None
        )
        self.assertEqual(legacy_set, f"[MED] OK SET {slot}")
        getq_after_set = await server.handle_medicine_command(f"MED GETQ {slot}", None)
        self.assertEqual(getq_after_set.removeprefix("[MED] OK GETQ ").split("|")[2], "5")

    async def test_listfull_inventory_protocol(self) -> None:
        records = [server.make_blank_medicine_record(slot) for slot in range(32)]
        records[1] = {
            "valid": True, "slot": 1, "quantity": 3, "name": "药A",
            "production_date": "2026-01-01", "shelf_life": "12个月", "efficacy": "测试",
        }
        records[7] = {
            "valid": True, "slot": 7, "quantity": 1, "name": "药|B\r\n胶囊",
            "production_date": "2025-12-01", "shelf_life": "24个月", "efficacy": "测试",
        }
        server.save_medicine_records(records)

        reply = await server.handle_medicine_command("MED LISTFULL", None)
        self.assertEqual(
            reply,
            "[MED] OK LISTFULL 药A|3|2026-01-01|12个月||药 B 胶囊|1|2025-12-01|24个月",
        )

        max_records = [
            {
                "valid": True, "slot": slot, "quantity": 999,
                "name": "💊" * 47, "production_date": "📅" * 15,
                "shelf_life": "⏳" * 23, "efficacy": "测试",
            }
            for slot in range(32)
        ]
        server.save_medicine_records(max_records)
        max_reply = await server.handle_medicine_command("MED LISTFULL", None)
        self.assertEqual(max_reply.count("||"), 31)
        self.assertLess(len(max_reply.encode("utf-8")), 12 * 1024)

        server.save_medicine_records([server.make_blank_medicine_record(slot) for slot in range(32)])
        self.assertEqual(
            await server.handle_medicine_command("MED LISTFULL", None),
            "[MED] OK LISTFULL EMPTY",
        )

    async def test_legacy_http_post_defaults_one_and_put_preserves_quantity(self) -> None:
        headers = {"x-fridge-token": server.APP_TOKEN}
        create_body = json.dumps({
            "name": "Legacy medicine",
            "production_date": "2026-01-01",
            "shelf_life": "12 months",
            "efficacy": "test",
        }).encode("utf-8")
        status, payload = await server.handle_http_api(
            "POST", "/api/medicines", headers, create_body
        )
        self.assertEqual(status, 201)
        self.assertEqual(payload["record"]["quantity"], 1)
        slot = payload["record"]["slot"]

        status, payload = await server.handle_http_api(
            "PATCH", f"/api/medicines/{slot}/quantity", headers,
            b'{"operation":"restock","value":4}',
        )
        self.assertEqual(status, 200)
        self.assertEqual(payload["quantity"], 5)

        update_body = json.dumps({
            "name": "Legacy medicine updated",
            "production_date": "2026-02-01",
            "shelf_life": "18 months",
            "efficacy": "updated",
        }).encode("utf-8")
        status, payload = await server.handle_http_api(
            "PUT", f"/api/medicines/{slot}", headers, update_body
        )
        self.assertEqual(status, 200)
        self.assertEqual(payload["record"]["quantity"], 5)

    async def test_mobile_health_api_returns_diagnosis_and_medicine(self) -> None:
        headers = {"x-fridge-token": server.APP_TOKEN}
        body = json.dumps({"symptom": "头疼发烧"}, ensure_ascii=False).encode("utf-8")

        with patch.object(server, "make_client", return_value=object()), patch.object(
            server,
            "ask_health_assessment",
            new=AsyncMock(
                return_value=server.HealthAssessment(
                    server.HealthStatus.NORMAL,
                    "感冒",
                    "999感冒灵",
                    "记得多喝温水哦",
                )
            ),
        ):
            status, payload = await server.handle_http_api(
                "POST", "/api/health/recommend", headers, body
            )

        self.assertEqual(status, 200)
        self.assertEqual(payload["diagnosis"], "感冒")
        self.assertEqual(payload["medicine"], "999感冒灵")
        self.assertEqual(payload["result"], "999感冒灵")
        record = server.dashboard_store.health(1)[0]
        self.assertEqual(record["diagnosis"], "感冒")
        self.assertEqual(record["medicine"], "999感冒灵")
        self.assertTrue(record["success"])

    async def test_mobile_health_failure_is_saved_to_history(self) -> None:
        headers = {"x-fridge-token": server.APP_TOKEN}
        body = json.dumps({"symptom": "头疼发烧"}, ensure_ascii=False).encode("utf-8")

        with patch.object(server, "make_client", return_value=object()), patch.object(
            server,
            "ask_health_assessment",
            new=AsyncMock(side_effect=RuntimeError("AI unavailable")),
        ):
            status, payload = await server.handle_http_api(
                "POST", "/api/health/recommend", headers, body
            )

        self.assertEqual(status, 500)
        self.assertIn("AI unavailable", payload["error"])
        record = server.dashboard_store.health(1)[0]
        self.assertEqual(record["diagnosis"], "诊断失败")
        self.assertEqual(record["medicine"], "无对应药品")
        self.assertFalse(record["success"])

    async def test_quantity_boundaries_and_auto_clear(self) -> None:
        slot = 0
        self.assertEqual(
            await server.handle_medicine_command(f"MED QTY {slot}|2", None),
            f"[MED] OK QTY {slot}|2",
        )
        self.assertEqual(
            await server.handle_medicine_command(f"MED TAKE {slot}|3", None),
            "[MED] ERR INSUFFICIENT",
        )
        self.assertEqual(
            await server.handle_medicine_command(f"MED RESTOCK {slot}|999", None),
            "[MED] ERR RANGE",
        )
        self.assertEqual(
            await server.handle_medicine_command(f"MED TAKE {slot}|2", None),
            f"[MED] OK TAKE {slot}|0",
        )
        self.assertEqual(
            await server.handle_medicine_command(f"MED GETQ {slot}", None),
            "[MED] ERR EMPTY",
        )
        self.assertEqual(
            await server.handle_medicine_command(f"MED RESTOCK {slot}|1", None),
            "[MED] ERR EMPTY",
        )

    async def test_http_quantity_patch_and_concurrent_restock(self) -> None:
        slot = 0
        headers = {"x-fridge-token": server.APP_TOKEN}
        status, payload = await server.handle_http_api(
            "PATCH", f"/api/medicines/{slot}/quantity", headers,
            b'{"operation":"set","value":1}',
        )
        self.assertEqual(status, 200)
        self.assertEqual(payload["quantity"], 1)

        async def restock_once() -> tuple[int, dict]:
            return await server.handle_http_api(
                "PATCH", f"/api/medicines/{slot}/quantity", headers,
                b'{"operation":"restock","value":1}',
            )

        results = await asyncio.gather(*(restock_once() for _ in range(20)))
        self.assertTrue(all(status_code == 200 for status_code, _ in results))
        records = server.load_medicine_records()
        self.assertEqual(records[slot]["quantity"], 21)

        status, payload = await server.handle_http_api(
            "PATCH", f"/api/medicines/{slot}/quantity", headers,
            b'{"operation":"take","value":21}',
        )
        self.assertEqual(status, 200)
        self.assertEqual(payload["quantity"], 0)
        self.assertFalse(payload["record"]["valid"])

    async def test_offline_snapshot_exposes_zero_without_destroying_last_temperature(self) -> None:
        server.fridge_state.update({
            "last_seen": time.time() - server.FRIDGE_ONLINE_TIMEOUT_SECONDS - 1,
            "current_temp": -4.6,
            "power_total_w": 39.6,
            "network_rtt_ms": 37.0,
        })
        snapshot = server.fridge_snapshot_unlocked()
        self.assertFalse(snapshot["online"])
        self.assertEqual(snapshot["current_temp"], 0.0)
        self.assertIsNone(snapshot["power_total_w"])
        self.assertIsNone(snapshot["network_rtt_ms"])
        self.assertEqual(server.fridge_state["current_temp"], -4.6)


async def run_preview() -> None:
    """Run the real HTTP handler with deterministic demo data for visual QA."""
    preview_root = Path(tempfile.mkdtemp(prefix="fridge-dashboard-preview-"))
    server.MEDICINE_DB_PATH = preview_root / "medicine_store.json"
    server.DASHBOARD_WEB_ROOT = Path(__file__).with_name("web")
    records = [server.make_blank_medicine_record(slot) for slot in range(32)]
    names = [
        "连花清瘟胶囊", "晚A茶", "感冒灵颗粒", "氨麻苯美片", "头孢克肟胶囊",
        "复方黄连素片", "氯雷他定片", "强力枇杷露", "布洛芬缓释胶囊", "维生素C片",
        "蒙脱石散", "阿莫西林胶囊", "藿香正气水", "板蓝根颗粒", "医用退热贴", "葡萄糖口服液",
    ]
    for slot, name in enumerate(names):
        records[slot] = {
            "valid": True, "slot": slot, "name": name,
            "quantity": (slot % 5) + 1,
            "production_date": "2026-01-15", "shelf_life": "2年", "efficacy": "演示数据",
        }
    records[4]["production_date"], records[4]["shelf_life"] = "2026-06-30", "30天"
    records[7]["production_date"], records[7]["shelf_life"] = "2024-01-01", "2年"
    records[10]["production_date"], records[10]["shelf_life"] = "未知", "未知"
    server.save_medicine_records(records)

    store = DashboardStore(preview_root / "dashboard.db", model="deepseek-v4-pro", tcp_port=10086, http_port=18087)
    server.dashboard_store = store
    now = time.time()
    server.fridge_state.update({
        "online": True, "last_seen": now, "set_temp": -3, "current_temp": -2.7,
        "fan": 2, "cool": True, "door": False, "water_cool": True, "vision_slot": 2,
        "vision_name": "感冒灵颗粒", "vision_updated_at": now,
        "power_40_w": 47.2, "power_41_w": 44.8, "power_total_w": 92.0,
        "network_rtt_ms": 42.0, "heartbeat_gap_ms": 1004.0,
    })
    preview_client = store.create_client("答辩手机", "preview-token-hash", "fc_preview…", "演示在线客户端")
    store.touch_client(preview_client["id"], timestamp=now)
    for index in range(121):
        ts = now - (120 - index) * 30
        current = -3.0 + math.sin(index / 9) * 0.8 + math.sin(index / 23) * 0.25
        power_40 = 46.0 + math.sin(index / 7) * 3.2
        power_41 = 43.5 + math.sin(index / 8 + 0.6) * 2.8
        rtt = 38.0 + abs(math.sin(index / 11)) * 18.0
        store.connection.execute(
            """INSERT INTO telemetry(
                ts,online,set_temp,current_temp,fan,cool,door,vision_slot,vision_name,
                power_40_w,power_41_w,power_total_w,network_rtt_ms,heartbeat_gap_ms
            ) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)""",
            (ts, 1, -3, current, 2, 1, 0, 2, "感冒灵颗粒",
             power_40, power_41, power_40 + power_41, rtt, 1000.0 + index % 4 * 12),
        )
    store.connection.commit()
    store.add_health("头部症状：头疼 眩晕，伴随轻微发热", "上呼吸道感染", "感冒灵颗粒", True, 642.4)
    store.add_health("鼻塞 流涕 咽部不适", "普通感冒", "连花清瘟胶囊", True, 518.2)
    store.record_metric("w800", "health_assess", True, 642.4)
    store.record_metric("w800", "fridge_status", True, 18.6)
    store.add_event("network", "success", "W800 TCP连接已建立", "10086设备通道已连接")
    store.add_event("vision", "info", "视觉识别：感冒灵颗粒", "槽位 3")
    store.add_event("ai", "success", "AI初筛：上呼吸道感染", "库存推荐：感冒灵颗粒")
    store.add_event("inventory", "warning", "药品即将到期", "头孢克肟胶囊剩余15天")

    async def keep_preview_online() -> None:
        while True:
            server.fridge_state["last_seen"] = time.time()
            await asyncio.sleep(1)

    http_server = await asyncio.start_server(server.handle_http_client, "127.0.0.1", 18087)
    heartbeat_task = asyncio.create_task(keep_preview_online())
    print("Dashboard preview: http://127.0.0.1:18087/", flush=True)
    try:
        async with http_server:
            await http_server.serve_forever()
    finally:
        heartbeat_task.cancel()
        store.close()


if __name__ == "__main__":
    if "--preview" in sys.argv:
        asyncio.run(run_preview())
    else:
        unittest.main()
