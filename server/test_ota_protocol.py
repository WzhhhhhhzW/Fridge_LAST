from __future__ import annotations

import tempfile
import unittest
import zlib
from pathlib import Path

import fridge_deepseek_server as server
from dashboard import DashboardStore


class MemoryWriter:
    def __init__(self) -> None:
        self.data = bytearray()
        self.closed = False

    def write(self, data: bytes) -> None:
        self.data.extend(data)

    async def drain(self) -> None:
        return None

    def is_closing(self) -> bool:
        return self.closed

    def close(self) -> None:
        self.closed = True


class OtaProtocolTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self) -> None:
        self.tempdir = tempfile.TemporaryDirectory()
        root = Path(self.tempdir.name)
        self.original_store = server.dashboard_store
        self.original_root = server.FIRMWARE_ROOT
        self.original_token = server.OTA_DEVICE_TOKEN
        self.original_device_writers = dict(server.OTA_DEVICE_WRITERS)
        server.OTA_DEVICE_WRITERS.clear()
        server.dashboard_store = DashboardStore(
            root / "dashboard.db",
            model="test",
            tcp_port=10086,
            http_port=10087,
        )
        server.FIRMWARE_ROOT = root / "firmware"
        server.FIRMWARE_ROOT.mkdir()
        server.OTA_DEVICE_TOKEN = "ota-test-token"

        release = server.dashboard_store.create_firmware_release(
            "main",
            "1.1.0+0",
            "OTA test",
            published=True,
        )
        self.image = bytes(index % 251 for index in range(9000))
        stored_name = "image.bin"
        (server.FIRMWARE_ROOT / stored_name).write_bytes(self.image)
        import hashlib
        self.sha256 = hashlib.sha256(self.image).hexdigest()
        artifact = server.dashboard_store.add_firmware_artifact(
            release["id"],
            "ota",
            "image.bin",
            stored_name,
            len(self.image),
            self.sha256,
            target_part=server.OTA_TARGET_PART,
            image_version="1.1.0+0",
            min_bootloader="1.0.0+0",
            signature_type="ecdsa-p256",
            image_valid=True,
        )
        self.artifact_id = artifact["id"]
        server.dashboard_store.upsert_ota_device(
            server.OTA_DEVICE_ID,
            server.OTA_TARGET_PART,
            "1.0.0+0",
            "1.0.0+0",
            0,
        )
        self.deployment = server.dashboard_store.create_ota_deployment(
            "deploy-test",
            server.OTA_DEVICE_ID,
            self.artifact_id,
            "1.0.0+0",
            "1.1.0+0",
            len(self.image),
            "admin:test",
        )

    async def asyncTearDown(self) -> None:
        if server.dashboard_store is not None:
            server.dashboard_store.close()
        server.dashboard_store = self.original_store
        server.FIRMWARE_ROOT = self.original_root
        server.OTA_DEVICE_TOKEN = self.original_token
        server.OTA_DEVICE_WRITERS.clear()
        server.OTA_DEVICE_WRITERS.update(self.original_device_writers)
        self.tempdir.cleanup()

    async def test_check_offers_valid_newer_image(self) -> None:
        reply = await server.handle_fw1_check([
            "FW1", "CHECK", server.OTA_DEVICE_ID, server.OTA_TARGET_PART,
            "1.0.0+0", "1.0.0+0", "0", "ota-test-token",
        ])
        self.assertEqual(
            reply,
            f"FW1 OFFER deploy-test 1.1.0+0 {len(self.image)} {self.sha256} 4096",
        )

    async def test_registered_connection_is_closed_to_interrupt_audio(self) -> None:
        writer = MemoryWriter()
        await server.handle_fw1_command(
            None,
            writer,
            (
                f"FW1 CHECK {server.OTA_DEVICE_ID} {server.OTA_TARGET_PART} "
                "1.0.0+0 1.0.0+0 0 ota-test-token"
            ),
        )
        self.assertIs(server.OTA_DEVICE_WRITERS[server.OTA_DEVICE_ID], writer)
        self.assertTrue(server.request_ota_device_reconnect(server.OTA_DEVICE_ID))
        self.assertTrue(writer.closed)
        self.assertFalse(server.request_ota_device_reconnect(server.OTA_DEVICE_ID))

    async def test_check_rejects_bad_token(self) -> None:
        with self.assertRaisesRegex(server.FirmwareProtocolError, "Invalid OTA device token"):
            await server.handle_fw1_check([
                "FW1", "CHECK", server.OTA_DEVICE_ID, server.OTA_TARGET_PART,
                "1.0.0+0", "1.0.0+0", "0", "wrong",
            ])

    async def test_check_rejects_unknown_hardware(self) -> None:
        with self.assertRaisesRegex(server.FirmwareProtocolError, "part number"):
            await server.handle_fw1_check([
                "FW1", "CHECK", server.OTA_DEVICE_ID, "R7FA6M4AF3CFB",
                "1.0.0+0", "1.0.0+0", "0", "ota-test-token",
            ])

    async def test_check_rejects_non_newer_version_and_old_bootloader(self) -> None:
        with self.assertRaisesRegex(server.FirmwareProtocolError, "not newer"):
            await server.handle_fw1_check([
                "FW1", "CHECK", server.OTA_DEVICE_ID, server.OTA_TARGET_PART,
                "1.1.0+0", "1.0.0+0", "0", "ota-test-token",
            ])

        with self.assertRaisesRegex(server.FirmwareProtocolError, "too old"):
            await server.handle_fw1_check([
                "FW1", "CHECK", server.OTA_DEVICE_ID, server.OTA_TARGET_PART,
                "1.0.0+0", "0.9.0+0", "0", "ota-test-token",
            ])

    async def test_invalid_artifact_is_never_offered(self) -> None:
        server.dashboard_store.connection.execute(
            "UPDATE firmware_artifacts SET image_valid = 0 WHERE id = ?",
            (self.artifact_id,),
        )
        server.dashboard_store.connection.commit()
        with self.assertRaisesRegex(server.FirmwareProtocolError, "not valid"):
            await server.handle_fw1_check([
                "FW1", "CHECK", server.OTA_DEVICE_ID, server.OTA_TARGET_PART,
                "1.0.0+0", "1.0.0+0", "0", "ota-test-token",
            ])
        self.assertEqual(
            server.dashboard_store.ota_deployment("deploy-test")["status"],
            "failed",
        )

    async def test_duplicate_active_deployment_is_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "device_has_active_deployment"):
            server.dashboard_store.create_ota_deployment(
                "deploy-duplicate",
                server.OTA_DEVICE_ID,
                self.artifact_id,
                "1.0.0+0",
                "1.1.0+0",
                len(self.image),
                "admin:test",
            )

    async def test_streams_chunks_and_supports_resume(self) -> None:
        writer = MemoryWriter()
        await server.handle_fw1_start(
            ["FW1", "START", "deploy-test", "4096", "ota-test-token"],
            writer,
        )
        output = bytes(writer.data)
        self.assertIn(b"FW1 DATA deploy-test 1 4096 4096 ", output)
        self.assertIn(b"FW1 END deploy-test 9000 ", output)
        self.assertEqual(
            server.dashboard_store.ota_deployment("deploy-test")["status"],
            "rebooting",
        )

        first_header, remainder = output.split(b"\n", 1)
        header_fields = first_header.decode("ascii").split()
        chunk_length = int(header_fields[5])
        expected_crc = int(header_fields[6], 16)
        first_chunk = remainder[:chunk_length]
        self.assertEqual(first_chunk, self.image[4096:8192])
        self.assertEqual(zlib.crc32(first_chunk) & 0xFFFFFFFF, expected_crc)

    async def test_rejects_unaligned_resume_offset(self) -> None:
        with self.assertRaisesRegex(server.FirmwareProtocolError, "aligned"):
            await server.handle_fw1_start(
                ["FW1", "START", "deploy-test", "1024", "ota-test-token"],
                MemoryWriter(),
            )

    async def test_offline_task_stays_queued_until_next_check(self) -> None:
        server.dashboard_store.connection.execute(
            "UPDATE ota_devices SET last_seen = 0 WHERE device_id = ?",
            (server.OTA_DEVICE_ID,),
        )
        server.dashboard_store.connection.commit()
        self.assertFalse(server.dashboard_store.ota_devices()[0]["online"])
        self.assertEqual(
            server.dashboard_store.ota_deployment("deploy-test")["status"],
            "queued",
        )

        reply = await server.handle_fw1_check([
            "FW1", "CHECK", server.OTA_DEVICE_ID, server.OTA_TARGET_PART,
            "1.0.0+0", "1.0.0+0", "0", "ota-test-token",
        ])
        self.assertTrue(reply.startswith("FW1 OFFER deploy-test "))
        self.assertTrue(server.dashboard_store.ota_devices()[0]["online"])

    async def test_progress_and_success_result(self) -> None:
        progress = await server.handle_fw1_progress([
            "FW1", "PROGRESS", "deploy-test", "4096", "writing", "ota-test-token",
        ])
        self.assertEqual(progress, "FW1 OK deploy-test")
        result = await server.handle_fw1_result([
            "FW1", "RESULT", "deploy-test", "1.1.0+0", "1",
            "success", "-", "ota-test-token",
        ])
        self.assertEqual(result, "FW1 OK deploy-test")
        self.assertEqual(
            server.dashboard_store.ota_deployment("deploy-test")["status"],
            "succeeded",
        )
        self.assertEqual(
            server.dashboard_store.ota_device(server.OTA_DEVICE_ID)["app_version"],
            "1.1.0+0",
        )

    async def test_trial_and_rollback_results_are_tracked(self) -> None:
        server.dashboard_store.update_ota_deployment(
            "deploy-test",
            status="rebooting",
            bytes_sent=len(self.image),
        )
        reply = await server.handle_fw1_check([
            "FW1", "CHECK", server.OTA_DEVICE_ID, server.OTA_TARGET_PART,
            "1.1.0+0", "1.0.0+0", "1", "ota-test-token",
        ])
        self.assertEqual(reply, "FW1 NONE")
        self.assertEqual(
            server.dashboard_store.ota_deployment("deploy-test")["status"],
            "trial",
        )

        result = await server.handle_fw1_result([
            "FW1", "RESULT", "deploy-test", "1.0.0+0", "0",
            "rolled_back", "trial_timeout", "ota-test-token",
        ])
        self.assertEqual(result, "FW1 OK deploy-test")
        deployment = server.dashboard_store.ota_deployment("deploy-test")
        self.assertEqual(deployment["status"], "rolled_back")
        self.assertEqual(deployment["error_code"], "trial_timeout")


if __name__ == "__main__":
    unittest.main()
