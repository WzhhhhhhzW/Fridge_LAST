from __future__ import annotations

import asyncio
import json
import sys
import types
import unittest
import zlib
from unittest.mock import AsyncMock, patch

from speech_pipeline import SpeechConfig, SpeechPipeline, SpeechPipelineError

if "openai" not in sys.modules:
    openai_stub = types.ModuleType("openai")

    class AsyncOpenAI:
        def __init__(self, *args, **kwargs):
            pass

    openai_stub.AsyncOpenAI = AsyncOpenAI
    sys.modules["openai"] = openai_stub

import fridge_deepseek_server as server


class FakeWebSocket:
    def __init__(self, mode: str, *, fail: bool = False) -> None:
        self.mode = mode
        self.fail = fail
        self.messages: asyncio.Queue[str | bytes] = asyncio.Queue()
        self.binary_sent: list[bytes] = []
        self.tts_text: str | None = None

    async def __aenter__(self) -> "FakeWebSocket":
        return self

    async def __aexit__(self, *_args: object) -> None:
        return None

    async def send(self, message: str | bytes) -> None:
        if isinstance(message, bytes):
            self.binary_sent.append(message)
            return

        event = json.loads(message)
        action = event["header"]["action"]
        if action == "run-task":
            await self.messages.put(
                json.dumps({"header": {"event": "task-started"}, "payload": {}})
            )
        elif action == "continue-task":
            self.tts_text = event["payload"]["input"]["text"]
        elif action == "finish-task" and self.fail:
            await self.messages.put(
                json.dumps(
                    {
                        "header": {
                            "event": "task-failed",
                            "error_code": "FakeFailure",
                            "error_message": "forced failure",
                        },
                        "payload": {},
                    }
                )
            )
        elif action == "finish-task" and self.mode == "asr":
            await self.messages.put(
                json.dumps(
                    {
                        "header": {"event": "result-generated"},
                        "payload": {
                            "output": {
                                "sentence": {
                                    "sentence_id": 1,
                                    "text": "这只是中间结果",
                                    "sentence_end": False,
                                }
                            }
                        },
                    },
                    ensure_ascii=False,
                )
            )
            await self.messages.put(
                json.dumps(
                    {
                        "header": {"event": "result-generated"},
                        "payload": {
                            "output": {
                                "sentence": {
                                    "sentence_id": 2,
                                    "text": "世界！",
                                    "sentence_end": True,
                                }
                            }
                        },
                    },
                    ensure_ascii=False,
                )
            )
            await self.messages.put(
                json.dumps(
                    {
                        "header": {"event": "result-generated"},
                        "payload": {
                            "output": {
                                "sentence": {
                                    "sentence_id": 1,
                                    "text": " 你好，",
                                    "sentence_end": True,
                                }
                            }
                        },
                    },
                    ensure_ascii=False,
                )
            )
            await self.messages.put(
                json.dumps({"header": {"event": "task-finished"}, "payload": {}})
            )
        elif action == "finish-task" and self.mode == "tts":
            await self.messages.put(b"\x01\x02")
            await self.messages.put(b"\x03\x04")
            await self.messages.put(
                json.dumps({"header": {"event": "task-finished"}, "payload": {}})
            )

    async def recv(self) -> str | bytes:
        return await self.messages.get()


class FakeConnector:
    def __init__(self, *websockets: FakeWebSocket) -> None:
        self.websockets = list(websockets)
        self.calls: list[tuple[str, dict[str, str], float]] = []

    async def __call__(
        self,
        url: str,
        headers: dict[str, str],
        timeout: float,
    ) -> FakeWebSocket:
        self.calls.append((url, headers, timeout))
        return self.websockets.pop(0)


class SpeechPipelineTests(unittest.IsolatedAsyncioTestCase):
    async def test_asr_preserves_text_and_audio_order(self) -> None:
        websocket = FakeWebSocket("asr")
        pipeline = SpeechPipeline(
            SpeechConfig(api_key="test-key"),
            connector=FakeConnector(websocket),
        )
        session = pipeline.create_asr_session()
        session.start()
        await session.push(b"\x01\x00" * 1600)
        await session.push(b"\x02\x00" * 1600)
        transcript = await session.finish()

        self.assertEqual(" 你好，世界！", transcript)
        self.assertEqual(
            [b"\x01\x00" * 1600, b"\x02\x00" * 1600],
            websocket.binary_sent,
        )

    async def test_tts_receives_exact_transcript_and_streams_binary(self) -> None:
        websocket = FakeWebSocket("tts")
        pipeline = SpeechPipeline(
            SpeechConfig(api_key="test-key"),
            connector=FakeConnector(websocket),
        )
        transcript = " 保留前导空格、数字123和标点！"
        chunks = [chunk async for chunk in pipeline.synthesize(transcript)]

        self.assertEqual(transcript, websocket.tts_text)
        self.assertEqual([b"\x01\x02", b"\x03\x04"], chunks)

    async def test_task_failure_has_stable_error_code(self) -> None:
        websocket = FakeWebSocket("asr", fail=True)
        pipeline = SpeechPipeline(
            SpeechConfig(api_key="test-key"),
            connector=FakeConnector(websocket),
        )
        session = pipeline.create_asr_session()
        session.start()
        with self.assertRaisesRegex(SpeechPipelineError, "forced failure") as raised:
            await session.finish()
        self.assertEqual("FakeFailure", raised.exception.code)

    async def test_missing_api_key_is_rejected_before_connect(self) -> None:
        pipeline = SpeechPipeline(SpeechConfig(api_key=""))
        with self.assertRaisesRegex(SpeechPipelineError, "DASHSCOPE_API_KEY"):
            pipeline.create_asr_session()

    async def test_asr_task_start_timeout_is_reported(self) -> None:
        class NeverStartedWebSocket(FakeWebSocket):
            async def send(self, message: str | bytes) -> None:
                if isinstance(message, str):
                    event = json.loads(message)
                    if event["header"]["action"] == "run-task":
                        return
                await super().send(message)

        pipeline = SpeechPipeline(
            SpeechConfig(
                api_key="test-key",
                task_start_timeout=0.01,
                task_finish_timeout=0.1,
            ),
            connector=FakeConnector(NeverStartedWebSocket("asr")),
        )
        session = pipeline.create_asr_session()
        session.start()
        with self.assertRaises(SpeechPipelineError) as raised:
            await session.finish()
        self.assertEqual("task_start_timeout", raised.exception.code)


class FakeAsrSession:
    def __init__(self, transcript: str) -> None:
        self.transcript = transcript
        self.frames: list[bytes] = []
        self.started = False
        self.aborted = False

    def start(self) -> None:
        self.started = True

    async def push(self, pcm: bytes) -> None:
        self.frames.append(pcm)

    async def finish(self) -> str:
        return self.transcript

    async def abort(self) -> None:
        self.aborted = True


class FakeSpeechPipeline:
    def __init__(self, transcript: str, tts_chunks: list[bytes]) -> None:
        self.session = FakeAsrSession(transcript)
        self.tts_chunks = tts_chunks
        self.tts_text: str | None = None

    def create_asr_session(self) -> FakeAsrSession:
        return self.session

    async def synthesize(self, text: str):
        self.tts_text = text
        for chunk in self.tts_chunks:
            yield chunk


class FakeStreamWriter:
    def __init__(self) -> None:
        self.data = bytearray()
        self.closed = False

    def write(self, data: bytes) -> None:
        self.data.extend(data)

    async def drain(self) -> None:
        return None

    def get_extra_info(self, name: str):
        return ("test-device", 10086) if name == "peername" else None

    def close(self) -> None:
        self.closed = True

    async def wait_closed(self) -> None:
        return None


class VoiceHealthTests(unittest.IsolatedAsyncioTestCase):
    @staticmethod
    def inventory_record(
        name: str = "布洛芬",
        *,
        quantity: object = 2,
        production_date: str = "2026-01-01",
        shelf_life: str = "10年",
    ) -> dict[str, object]:
        return {
            "valid": True,
            "name": name,
            "quantity": quantity,
            "production_date": production_date,
            "shelf_life": shelf_life,
            "efficacy": "退烧止痛",
        }

    def test_inventory_filter_rejects_zero_expired_and_unknown_expiry(self) -> None:
        records = [
            self.inventory_record("可用药"),
            self.inventory_record("零库存", quantity=0),
            self.inventory_record(
                "过期药",
                production_date="2020-01-01",
                shelf_life="1年",
            ),
            self.inventory_record("未知期限", shelf_life="不知道"),
            self.inventory_record("坏数量", quantity="not-a-number"),
        ]

        eligible = server.eligible_health_inventory(records)

        self.assertEqual(["可用药"], [record["name"] for record in eligible])

    def test_inventory_name_must_match_exactly(self) -> None:
        records = [self.inventory_record()]
        self.assertEqual(
            "布洛芬",
            server.select_inventory_medicine("布洛芬", records),
        )
        self.assertEqual(
            server.HEALTH_NO_MEDICINE,
            server.select_inventory_medicine("布洛芬、阿司匹林", records),
        )
        self.assertEqual(
            server.HEALTH_NO_MEDICINE,
            server.select_inventory_medicine("库存外药品", records),
        )

    def test_voice_reply_has_only_three_permitted_shapes(self) -> None:
        self.assertEqual(
            "可能是--感冒，建议服用--布洛芬，记得多喝温水哦",
            server.format_voice_health_reply(
                server.HealthAssessment(
                    server.HealthStatus.NORMAL,
                    "感冒",
                    "布洛芬",
                    "记得多喝温水哦",
                )
            ),
        )
        self.assertEqual(
            "库存中没有对症药品",
            server.format_voice_health_reply(server.no_medicine_health_assessment()),
        )
        self.assertEqual(
            "请立刻拨打120",
            server.format_voice_health_reply(server.emergency_health_assessment()),
        )
        sanitized = server.format_voice_health_reply(
            server.HealthAssessment(
                server.HealthStatus.NORMAL,
                "感冒--建议服用|其他药\n第二行",
                "布洛芬",
                "注意休息哦",
            )
        )
        self.assertEqual(2, sanitized.count("--"))
        self.assertNotIn("|", sanitized)
        self.assertNotIn("\n", sanitized)

    def test_invalid_care_phrases_use_contextual_fallback(self) -> None:
        for value in (
            "这是一句明显超过十个字的关心提醒",
            "多喝水|伪造协议",
            "每天服用2片",
            "",
        ):
            phrase = server.normalize_health_care_phrase(value, "我发烧咳嗽", "感冒")
            self.assertEqual("记得多喝温水哦", phrase)
            self.assertLessEqual(len(phrase), server.HEALTH_CARE_MAX_CHARS)

        self.assertEqual(
            "饮食清淡些哦",
            server.normalize_health_care_phrase("", "胃不舒服", "肠胃不适"),
        )

    async def test_assessment_is_one_shot_and_receives_only_eligible_inventory(self) -> None:
        create = AsyncMock(
            return_value=types.SimpleNamespace(
                choices=[
                    types.SimpleNamespace(
                        message=types.SimpleNamespace(
                            content="普通|感冒|布洛芬|记得多喝温水哦"
                        )
                    )
                ]
            )
        )
        client = types.SimpleNamespace(
            chat=types.SimpleNamespace(
                completions=types.SimpleNamespace(create=create)
            )
        )
        records = [
            self.inventory_record(),
            self.inventory_record("零库存", quantity=0),
        ]

        result = await server.ask_health_assessment(client, "我头痛，发烧", records)

        self.assertEqual(
            server.HealthAssessment(
                server.HealthStatus.NORMAL,
                "感冒",
                "布洛芬",
                "记得多喝温水哦",
            ),
            result,
        )
        messages = create.await_args.kwargs["messages"]
        self.assertEqual(["system", "user"], [item["role"] for item in messages])
        self.assertIn("我头痛，发烧", messages[1]["content"])
        self.assertIn("布洛芬: 退烧止痛", messages[1]["content"])
        self.assertNotIn("零库存", messages[1]["content"])

    async def test_explicit_emergencies_never_call_model(self) -> None:
        for symptom in (
            "我吐血了",
            "我刚刚吐了血",
            "我心脏痛",
            "我心脏疼得厉害",
            "突然胸痛并且呼吸困难",
            "家人昏迷且没有意识",
            "伤口大出血",
        ):
            create = AsyncMock()
            client = types.SimpleNamespace(
                chat=types.SimpleNamespace(
                    completions=types.SimpleNamespace(create=create)
                )
            )
            result = await server.ask_health_assessment(
                client,
                symptom,
                [self.inventory_record()],
            )

            self.assertEqual(server.emergency_health_assessment(), result)
            create.assert_not_awaited()

    async def test_negated_emergency_term_does_not_trigger_local_rule(self) -> None:
        create = AsyncMock(
            return_value=types.SimpleNamespace(
                choices=[
                    types.SimpleNamespace(
                        message=types.SimpleNamespace(
                            content="普通|感冒|布洛芬|记得多喝温水哦"
                        )
                    )
                ]
            )
        )
        client = types.SimpleNamespace(
            chat=types.SimpleNamespace(
                completions=types.SimpleNamespace(create=create)
            )
        )

        result = await server.ask_health_assessment(
            client,
            "我没有胸痛，只是轻微咳嗽",
            [self.inventory_record()],
        )

        self.assertEqual(server.HealthStatus.NORMAL, result.status)
        create.assert_awaited_once()

    async def test_model_emergency_is_replaced_with_fixed_alert(self) -> None:
        create = AsyncMock(
            return_value=types.SimpleNamespace(
                choices=[
                    types.SimpleNamespace(
                        message=types.SimpleNamespace(
                            content="急症|模型自由诊断|库存外药品|模型自由提醒"
                        )
                    )
                ]
            )
        )
        client = types.SimpleNamespace(
            chat=types.SimpleNamespace(
                completions=types.SimpleNamespace(create=create)
            )
        )

        result = await server.ask_health_assessment(
            client,
            "胸口有一种说不清的压迫感",
            [self.inventory_record()],
        )

        self.assertEqual(server.emergency_health_assessment(), result)
        self.assertEqual("", result.care_phrase)

    async def test_invalid_model_shape_degrades_to_no_medicine(self) -> None:
        create = AsyncMock(
            return_value=types.SimpleNamespace(
                choices=[
                    types.SimpleNamespace(
                        message=types.SimpleNamespace(content="感冒|布洛芬")
                    )
                ]
            )
        )
        client = types.SimpleNamespace(
            chat=types.SimpleNamespace(
                completions=types.SimpleNamespace(create=create)
            )
        )

        result = await server.ask_health_assessment(
            client,
            "我有一点不舒服",
            [self.inventory_record()],
        )

        self.assertEqual(server.no_medicine_health_assessment(), result)


class AudioProtocolTests(unittest.IsolatedAsyncioTestCase):
    async def test_audio_say_streams_exact_utf8_text_without_asr(self) -> None:
        medicine_name = "\u67b8\u6a7c\u9178\u83ab\u6c99\u5fc5\u5229\u7247"
        encoded_name = medicine_name.encode("utf-8")
        reader = asyncio.StreamReader()
        reader.feed_data(encoded_name)
        reader.feed_eof()
        writer = FakeStreamWriter()
        pipeline = FakeSpeechPipeline(
            "must-not-run-asr",
            [b"\x10\x00" * 1600, b"\x20\x00" * 100],
        )

        with (
            patch.object(server, "AUDIO_DEVICE_TOKEN", "device-secret"),
            patch.object(server, "speech_pipeline", pipeline),
        ):
            await server.handle_audio_say(
                reader,
                writer,  # type: ignore[arg-type]
                f"AUD1 SAY SAY1 {len(encoded_name)} device-secret",
            )

        response = bytes(writer.data)
        self.assertTrue(response.startswith(b"AUD1 READY SAY1\n"))
        self.assertIn(b"AUD1 PCM SAY1 0 3200 ", response)
        self.assertIn(b"AUD1 PCM SAY1 1 200 ", response)
        self.assertTrue(response.endswith(b"AUD1 DONE SAY1 2 3400\n"))
        self.assertEqual(medicine_name, pipeline.tts_text)
        self.assertFalse(pipeline.session.started)

    async def test_audio_say_rejects_wrong_token_before_tts(self) -> None:
        reader = asyncio.StreamReader()
        writer = FakeStreamWriter()
        pipeline = FakeSpeechPipeline("", [b"must-not-be-used"])

        with (
            patch.object(server, "AUDIO_DEVICE_TOKEN", "correct-secret"),
            patch.object(server, "speech_pipeline", pipeline),
            self.assertRaises(server.AudioProtocolError),
        ):
            await server.handle_audio_say(
                reader,
                writer,  # type: ignore[arg-type]
                "AUD1 SAY SAY2 3 wrong-secret",
            )

        self.assertIn(b"AUD1 ERROR SAY2 unauthorized", writer.data)
        self.assertIsNone(pipeline.tts_text)
        self.assertFalse(pipeline.session.started)

    async def test_audio_say_rejects_invalid_utf8(self) -> None:
        reader = asyncio.StreamReader()
        reader.feed_data(b"\xff\xfe")
        reader.feed_eof()
        writer = FakeStreamWriter()
        pipeline = FakeSpeechPipeline("", [b"must-not-be-used"])

        with (
            patch.object(server, "AUDIO_DEVICE_TOKEN", "secret"),
            patch.object(server, "speech_pipeline", pipeline),
            self.assertRaises(server.AudioProtocolError),
        ):
            await server.handle_audio_say(
                reader,
                writer,  # type: ignore[arg-type]
                "AUD1 SAY SAY3 2 secret",
            )

        self.assertIn(b"AUD1 ERROR SAY3 bad_utf8", writer.data)
        self.assertIsNone(pipeline.tts_text)

    async def test_audio_say_rejects_oversized_text_header(self) -> None:
        reader = asyncio.StreamReader()
        writer = FakeStreamWriter()

        with (
            patch.object(server, "AUDIO_DEVICE_TOKEN", "secret"),
            self.assertRaises(server.AudioProtocolError),
        ):
            await server.handle_audio_say(
                reader,
                writer,  # type: ignore[arg-type]
                "AUD1 SAY SAY4 161 secret",
            )

        self.assertIn(b"AUD1 ERROR SAY4 bad_number", writer.data)

    async def test_legacy_tcp_ping_still_works(self) -> None:
        reader = asyncio.StreamReader()
        reader.feed_data(b"/ping\n")
        reader.feed_eof()
        writer = FakeStreamWriter()

        with patch.object(server, "make_client", return_value=object()):
            await server.handle_client(reader, writer)  # type: ignore[arg-type]

        self.assertEqual(b"pong\r\n[END]\r\n", bytes(writer.data))
        self.assertTrue(writer.closed)

    async def test_audio_protocol_preserves_transcript_and_pcm(self) -> None:
        pcm = b"\x01\x00" * 1600
        checksum = zlib.crc32(pcm) & 0xFFFFFFFF
        reader = asyncio.StreamReader()
        reader.feed_data(
            f"AUD1 DATA A1B2C3D4 0 1600 {checksum:08X}\n".encode("ascii")
            + pcm
            + b"AUD1 END A1B2C3D4 1 1600\n"
        )
        writer = FakeStreamWriter()
        pipeline = FakeSpeechPipeline(
            " 原文123！",
            [b"\x10\x00" * 1000, b"\x20\x00" * 700],
        )

        with (
            patch.object(server, "AUDIO_DEVICE_TOKEN", "device-secret"),
            patch.object(server, "speech_pipeline", pipeline),
            patch.object(server, "load_medicine_records", return_value=[]),
            patch.object(
                server,
                "ask_health_assessment",
                new=AsyncMock(
                    return_value=server.HealthAssessment(
                        server.HealthStatus.NORMAL,
                        "感冒",
                        "布洛芬",
                        "记得多喝温水哦",
                    )
                ),
            ),
        ):
            await server.handle_audio_session(
                reader,
                writer,  # type: ignore[arg-type]
                "AUD1 START A1B2C3D4 16000 1 S16LE device-secret",
                object(),
            )

        response = bytes(writer.data)
        self.assertTrue(response.startswith(b"AUD1 READY A1B2C3D4\n"))
        self.assertIn("AUD1 TEXT A1B2C3D4 13\n 原文123！".encode("utf-8"), response)
        diagnosis = "感冒".encode("utf-8")
        medicine = "布洛芬".encode("utf-8")
        health_frame = (
            f"AUD1 HEALTH A1B2C3D4 {len(diagnosis)} {len(medicine)}\n".encode("ascii")
            + diagnosis
            + medicine
        )
        self.assertIn(health_frame, response)
        self.assertLess(response.index(health_frame), response.index(b"AUD1 PCM"))
        self.assertIn(b"AUD1 PCM A1B2C3D4 0 3200 ", response)
        self.assertIn(b"AUD1 PCM A1B2C3D4 1 200 ", response)
        self.assertTrue(response.endswith(b"AUD1 DONE A1B2C3D4 2 3400\n"))
        self.assertEqual(
            "可能是--感冒，建议服用--布洛芬，记得多喝温水哦",
            pipeline.tts_text,
        )
        self.assertEqual([pcm], pipeline.session.frames)

    async def test_bad_crc_closes_audio_session(self) -> None:
        pcm = b"\x00\x00" * 1600
        reader = asyncio.StreamReader()
        reader.feed_data(b"AUD1 DATA A1 0 1600 00000000\n" + pcm)
        writer = FakeStreamWriter()
        pipeline = FakeSpeechPipeline("", [])

        with (
            patch.object(server, "AUDIO_DEVICE_TOKEN", "secret"),
            patch.object(server, "speech_pipeline", pipeline),
            self.assertRaises(server.AudioProtocolError),
        ):
            await server.handle_audio_session(
                reader,
                writer,  # type: ignore[arg-type]
                "AUD1 START A1 16000 1 S16LE secret",
                object(),
            )

        self.assertIn(b"AUD1 ERROR A1 crc_mismatch", writer.data)
        self.assertTrue(pipeline.session.aborted)

    async def test_empty_audio_returns_empty_text_without_tts(self) -> None:
        reader = asyncio.StreamReader()
        reader.feed_data(b"AUD1 END EMPTY-1 0 0\n")
        writer = FakeStreamWriter()
        pipeline = FakeSpeechPipeline("", [b"must-not-be-used"])
        assessment = AsyncMock()

        with (
            patch.object(server, "AUDIO_DEVICE_TOKEN", "secret"),
            patch.object(server, "speech_pipeline", pipeline),
            patch.object(server, "ask_health_assessment", new=assessment),
        ):
            await server.handle_audio_session(
                reader,
                writer,  # type: ignore[arg-type]
                "AUD1 START EMPTY-1 16000 1 S16LE secret",
                object(),
            )

        self.assertEqual(
            b"AUD1 READY EMPTY-1\n"
            b"AUD1 TEXT EMPTY-1 0\n"
            b"AUD1 DONE EMPTY-1 0 0\n",
            bytes(writer.data),
        )
        self.assertIsNone(pipeline.tts_text)
        assessment.assert_not_awaited()

    async def test_audio_health_no_medicine_is_structured_before_tts(self) -> None:
        reader = asyncio.StreamReader()
        reader.feed_data(b"AUD1 END HEALTH-NONE 0 0\n")
        writer = FakeStreamWriter()
        pipeline = FakeSpeechPipeline("症状不明确", [b"\x10\x00" * 100])

        with (
            patch.object(server, "AUDIO_DEVICE_TOKEN", "secret"),
            patch.object(server, "speech_pipeline", pipeline),
            patch.object(server, "load_medicine_records", return_value=[]),
            patch.object(
                server,
                "ask_health_assessment",
                new=AsyncMock(return_value=server.no_medicine_health_assessment()),
            ),
        ):
            await server.handle_audio_session(
                reader,
                writer,  # type: ignore[arg-type]
                "AUD1 START HEALTH-NONE 16000 1 S16LE secret",
                object(),
            )

        diagnosis = "无法判断".encode("utf-8")
        medicine = "无对应药品".encode("utf-8")
        health_frame = (
            f"AUD1 HEALTH HEALTH-NONE {len(diagnosis)} {len(medicine)}\n".encode("ascii")
            + diagnosis
            + medicine
        )
        response = bytes(writer.data)
        self.assertIn(health_frame, response)
        self.assertLess(response.index(health_frame), response.index(b"AUD1 PCM"))
        self.assertEqual("库存中没有对症药品", pipeline.tts_text)

    async def test_audio_emergency_is_structured_and_tts_is_fixed_alert(self) -> None:
        reader = asyncio.StreamReader()
        reader.feed_data(b"AUD1 END HEALTH-EMERGENCY 0 0\n")
        writer = FakeStreamWriter()
        pipeline = FakeSpeechPipeline("我吐血了", [b"\x10\x00" * 100])

        with (
            patch.object(server, "AUDIO_DEVICE_TOKEN", "secret"),
            patch.object(server, "speech_pipeline", pipeline),
            patch.object(server, "load_medicine_records", return_value=[]),
        ):
            await server.handle_audio_session(
                reader,
                writer,  # type: ignore[arg-type]
                "AUD1 START HEALTH-EMERGENCY 16000 1 S16LE secret",
                object(),
            )

        diagnosis = "紧急情况".encode("utf-8")
        medicine = "请立刻拨打120".encode("utf-8")
        health_frame = (
            f"AUD1 HEALTH HEALTH-EMERGENCY {len(diagnosis)} {len(medicine)}\n".encode(
                "ascii"
            )
            + diagnosis
            + medicine
        )
        response = bytes(writer.data)
        self.assertIn(health_frame, response)
        self.assertLess(response.index(health_frame), response.index(b"AUD1 PCM"))
        self.assertEqual("请立刻拨打120", pipeline.tts_text)

    async def test_audio_health_rejects_control_characters(self) -> None:
        writer = FakeStreamWriter()
        with self.assertRaisesRegex(server.SpeechPipelineError, "control character"):
            await server.send_audio_health(
                writer,  # type: ignore[arg-type]
                "HEALTH-BAD",
                "感冒\n伪造协议",
                "布洛芬",
            )
        self.assertEqual(b"", bytes(writer.data))

    async def test_deepseek_failure_returns_stable_audio_error(self) -> None:
        reader = asyncio.StreamReader()
        reader.feed_data(b"AUD1 END HEALTH-FAIL 0 0\n")
        writer = FakeStreamWriter()
        pipeline = FakeSpeechPipeline("头痛发烧", [b"must-not-be-used"])

        with (
            patch.object(server, "AUDIO_DEVICE_TOKEN", "secret"),
            patch.object(server, "speech_pipeline", pipeline),
            patch.object(server, "load_medicine_records", return_value=[]),
            patch.object(
                server,
                "ask_health_assessment",
                new=AsyncMock(side_effect=TimeoutError("forced timeout")),
            ),
        ):
            await server.handle_audio_session(
                reader,
                writer,  # type: ignore[arg-type]
                "AUD1 START HEALTH-FAIL 16000 1 S16LE secret",
                object(),
            )

        self.assertIn(
            b"AUD1 ERROR HEALTH-FAIL health_assessment_failed",
            writer.data,
        )
        self.assertIsNone(pipeline.tts_text)

    async def test_wrong_token_is_rejected_before_asr(self) -> None:
        reader = asyncio.StreamReader()
        writer = FakeStreamWriter()
        pipeline = FakeSpeechPipeline("", [])

        with (
            patch.object(server, "AUDIO_DEVICE_TOKEN", "correct-secret"),
            patch.object(server, "speech_pipeline", pipeline),
        ):
            await server.handle_audio_session(
                reader,
                writer,  # type: ignore[arg-type]
                "AUD1 START TOKEN1 16000 1 S16LE wrong-secret",
                object(),
            )

        self.assertIn(b"AUD1 ERROR TOKEN1 unauthorized", writer.data)
        self.assertFalse(pipeline.session.started)

    async def test_out_of_order_sequence_is_rejected(self) -> None:
        pcm = b"\x00\x00" * 1600
        checksum = zlib.crc32(pcm) & 0xFFFFFFFF
        reader = asyncio.StreamReader()
        reader.feed_data(
            f"AUD1 DATA SEQ1 1 1600 {checksum:08X}\n".encode("ascii") + pcm
        )
        writer = FakeStreamWriter()
        pipeline = FakeSpeechPipeline("", [])

        with (
            patch.object(server, "AUDIO_DEVICE_TOKEN", "secret"),
            patch.object(server, "speech_pipeline", pipeline),
            self.assertRaises(server.AudioProtocolError),
        ):
            await server.handle_audio_session(
                reader,
                writer,  # type: ignore[arg-type]
                "AUD1 START SEQ1 16000 1 S16LE secret",
                object(),
            )

        self.assertIn(b"AUD1 ERROR SEQ1 sequence_mismatch", writer.data)

    async def test_end_length_mismatch_is_rejected(self) -> None:
        reader = asyncio.StreamReader()
        reader.feed_data(b"AUD1 END LENGTH1 1 1600\n")
        writer = FakeStreamWriter()
        pipeline = FakeSpeechPipeline("", [])

        with (
            patch.object(server, "AUDIO_DEVICE_TOKEN", "secret"),
            patch.object(server, "speech_pipeline", pipeline),
            self.assertRaises(server.AudioProtocolError),
        ):
            await server.handle_audio_session(
                reader,
                writer,  # type: ignore[arg-type]
                "AUD1 START LENGTH1 16000 1 S16LE secret",
                object(),
            )

        self.assertIn(b"AUD1 ERROR LENGTH1 length_mismatch", writer.data)


if __name__ == "__main__":
    unittest.main()
