from __future__ import annotations

import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit

from ota_packager_core import (
    FirmwareServerClient,
    MCUBOOT_SLOT_SIZE,
    PackagerError,
    inspect_mcuboot_image,
    normalize_version,
    read_source_version,
)


class OtaPackagerCoreTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.fridge_dir = Path(__file__).resolve().parent.parent

    def test_normalize_version(self) -> None:
        self.assertEqual(normalize_version("1.0.0+14"), "1.0.0+14")
        with self.assertRaises(PackagerError):
            normalize_version("1.0.14")

    def test_read_current_source_version(self) -> None:
        version = read_source_version(
            self.fridge_dir / "project_firdge" / "src" / "ota_update_config.h"
        )
        self.assertRegex(version, r"^\d+\.\d+\.\d+\+\d+$")

    def test_inspect_current_signed_image(self) -> None:
        image = (
            self.fridge_dir
            / "project_firdge"
            / "Debug"
            / "project_firdge_1.0.0+14_signed_keyhash.bin"
        )
        if not image.is_file():
            self.skipTest("current signed fixture is not present")
        result = inspect_mcuboot_image(image, "1.0.0+14")
        self.assertEqual(result["slot_bytes"], MCUBOOT_SLOT_SIZE)
        self.assertGreater(result["application_bytes"], 0)

    def test_rejects_wrong_size(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            bad = Path(directory) / "bad.bin"
            bad.write_bytes(b"not-an-image")
            with self.assertRaises(PackagerError):
                inspect_mcuboot_image(bad)


class _FirmwareApiHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    uploaded = b""

    def log_message(self, _format: str, *_args: object) -> None:
        pass

    def _json(self, status: int, payload: dict[str, object], *, cookie: bool = False) -> None:
        import json

        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        if cookie:
            self.send_header("Set-Cookie", "fridge_admin_session=test-session; Path=/; HttpOnly")
        self.end_headers()
        self.wfile.write(body)

    def _authorized(self) -> bool:
        expected_origin = f"http://{self.headers['Host']}"
        return (
            "fridge_admin_session=test-session" in self.headers.get("Cookie", "")
            and self.headers.get("X-CSRF-Token") == "test-csrf"
            and self.headers.get("Origin") == expected_origin
        )

    def do_GET(self) -> None:
        if self.path == "/api/admin/firmware/releases":
            self._json(200, {"ok": True, "releases": []})
        else:
            self._json(404, {"ok": False})

    def do_POST(self) -> None:
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        path = urlsplit(self.path).path
        if path == "/api/admin/login":
            self._json(
                200,
                {"ok": True, "authenticated": True, "csrf_token": "test-csrf"},
                cookie=True,
            )
        elif path == "/api/admin/firmware/releases" and self._authorized():
            self._json(201, {"ok": True, "release": {"id": 42, "version": "1.0.0+15", "published": True}})
        elif path == "/api/admin/firmware/artifacts" and self._authorized():
            type(self).uploaded = body
            self._json(
                201,
                {"ok": True, "artifact": {"id": 9, "image_valid": True, "image_version": "1.0.0+15"}},
            )
        else:
            self._json(403, {"ok": False, "error": "forbidden"})


class FirmwareServerClientTests(unittest.TestCase):
    def test_login_create_release_and_upload(self) -> None:
        server = ThreadingHTTPServer(("127.0.0.1", 0), _FirmwareApiHandler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            client = FirmwareServerClient(f"http://127.0.0.1:{server.server_port}")
            client.login("password")
            release = client.ensure_release("1.0.0+15", title="test", changelog="test")
            self.assertEqual(release["id"], 42)
            with tempfile.TemporaryDirectory() as directory:
                image = Path(directory) / "firmware.bin"
                image.write_bytes(b"signed-image-placeholder")
                artifact = client.upload_ota(42, image)
            self.assertTrue(artifact["image_valid"])
            self.assertEqual(_FirmwareApiHandler.uploaded, b"signed-image-placeholder")
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)


if __name__ == "__main__":
    unittest.main()
