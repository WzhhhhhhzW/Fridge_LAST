#!/usr/bin/env python3
"""DashScope Fun-ASR and CosyVoice WebSocket helpers.

The module intentionally has no dependency on the fridge TCP protocol.  It
accepts raw 16 kHz mono PCM16 frames and exposes the final ASR text plus a
stream of raw PCM16 TTS bytes.
"""

from __future__ import annotations

import asyncio
import json
import os
import uuid
from dataclasses import dataclass
from typing import Any, AsyncIterator, Callable


class SpeechPipelineError(RuntimeError):
    """A cloud speech task failed or returned an invalid event."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


@dataclass(frozen=True)
class SpeechConfig:
    api_key: str
    websocket_url: str = "wss://dashscope.aliyuncs.com/api-ws/v1/inference/"
    workspace_id: str = ""
    asr_model: str = "fun-asr-realtime"
    tts_model: str = "cosyvoice-v3-flash"
    tts_voice: str = "longxiaochun_v3"
    sample_rate: int = 16000
    task_start_timeout: float = 15.0
    task_finish_timeout: float = 30.0
    audio_queue_frames: int = 300

    @classmethod
    def from_env(cls) -> "SpeechConfig":
        return cls(
            api_key=os.getenv("DASHSCOPE_API_KEY", ""),
            websocket_url=os.getenv(
                "DASHSCOPE_WS_URL",
                "wss://dashscope.aliyuncs.com/api-ws/v1/inference/",
            ),
            workspace_id=os.getenv("DASHSCOPE_WORKSPACE_ID", ""),
            asr_model=os.getenv("FRIDGE_ASR_MODEL", "fun-asr-realtime"),
            tts_model=os.getenv("FRIDGE_TTS_MODEL", "cosyvoice-v3-flash"),
            tts_voice=os.getenv("FRIDGE_TTS_VOICE", "longxiaochun_v3"),
        )


async def _default_connect(url: str, headers: dict[str, str], timeout: float) -> Any:
    """Open a websocket while supporting both old and new websockets APIs."""

    try:
        from websockets.asyncio.client import connect
    except ImportError as exc:
        raise SpeechPipelineError(
            "dependency_missing",
            "Python dependency 'websockets' is not installed",
        ) from exc

    try:
        return await connect(url, additional_headers=headers, open_timeout=timeout)
    except TypeError:
        # websockets <= 13 used extra_headers instead of additional_headers.
        return await connect(url, extra_headers=headers, open_timeout=timeout)


Connector = Callable[[str, dict[str, str], float], Any]
_AUDIO_END = object()


def _event(message: Any) -> dict[str, Any]:
    if isinstance(message, bytes):
        raise SpeechPipelineError("unexpected_binary", "Expected a JSON event")
    try:
        value = json.loads(message)
    except (TypeError, json.JSONDecodeError) as exc:
        raise SpeechPipelineError("invalid_event", "Cloud returned invalid JSON") from exc
    if not isinstance(value, dict):
        raise SpeechPipelineError("invalid_event", "Cloud event must be an object")
    return value


def _event_name(event: dict[str, Any]) -> str:
    header = event.get("header")
    return str(header.get("event", "")) if isinstance(header, dict) else ""


def _raise_task_failed(event: dict[str, Any]) -> None:
    header = event.get("header")
    if not isinstance(header, dict):
        raise SpeechPipelineError("task_failed", "Cloud speech task failed")
    raise SpeechPipelineError(
        str(header.get("error_code", "task_failed")),
        str(header.get("error_message", "Cloud speech task failed")),
    )


class FunAsrSession:
    """One live Fun-ASR task fed from an asyncio queue."""

    def __init__(self, pipeline: "SpeechPipeline") -> None:
        self._pipeline = pipeline
        self._queue: asyncio.Queue[bytes | object] = asyncio.Queue(
            maxsize=pipeline.config.audio_queue_frames
        )
        self._task: asyncio.Task[str] | None = None
        self._finished = False

    def start(self) -> None:
        if self._task is not None:
            raise SpeechPipelineError("invalid_state", "ASR session already started")
        self._task = asyncio.create_task(self._run(), name="fun-asr-session")

    async def push(self, pcm: bytes) -> None:
        if self._finished or self._task is None:
            raise SpeechPipelineError("invalid_state", "ASR session is not accepting audio")
        if self._task.done():
            await self._task
        try:
            self._queue.put_nowait(bytes(pcm))
        except asyncio.QueueFull as exc:
            raise SpeechPipelineError("audio_queue_full", "ASR audio queue is full") from exc

    async def finish(self) -> str:
        if self._task is None:
            raise SpeechPipelineError("invalid_state", "ASR session was not started")
        if not self._finished:
            self._finished = True
            await self._queue.put(_AUDIO_END)
        try:
            return await asyncio.wait_for(
                self._task,
                timeout=self._pipeline.config.task_finish_timeout,
            )
        except TimeoutError as exc:
            self._task.cancel()
            raise SpeechPipelineError("asr_finish_timeout", "ASR finish timed out") from exc

    async def abort(self) -> None:
        self._finished = True
        if self._task is not None and not self._task.done():
            self._task.cancel()
            try:
                await self._task
            except (asyncio.CancelledError, SpeechPipelineError):
                pass

    async def _run(self) -> str:
        task_id = str(uuid.uuid4())
        websocket = await self._pipeline._connect()
        async with websocket:
            await websocket.send(
                json.dumps(
                    {
                        "header": {
                            "action": "run-task",
                            "task_id": task_id,
                            "streaming": "duplex",
                        },
                        "payload": {
                            "task_group": "audio",
                            "task": "asr",
                            "function": "recognition",
                            "model": self._pipeline.config.asr_model,
                            "parameters": {
                                "format": "pcm",
                                "sample_rate": self._pipeline.config.sample_rate,
                            },
                            "input": {},
                        },
                    },
                    ensure_ascii=False,
                )
            )

            await self._pipeline._wait_for_started(websocket)
            receiver = asyncio.create_task(
                self._receive_results(websocket),
                name="fun-asr-results",
            )
            try:
                while True:
                    audio = await self._queue.get()
                    if audio is _AUDIO_END:
                        break
                    await websocket.send(audio)

                await websocket.send(
                    json.dumps(
                        {
                            "header": {
                                "action": "finish-task",
                                "task_id": task_id,
                                "streaming": "duplex",
                            },
                            "payload": {"input": {}},
                        }
                    )
                )
                return await receiver
            except BaseException:
                receiver.cancel()
                try:
                    await receiver
                except (asyncio.CancelledError, SpeechPipelineError):
                    pass
                raise

    async def _receive_results(self, websocket: Any) -> str:
        sentences: dict[int, str] = {}
        while True:
            message = await websocket.recv()
            event = _event(message)
            name = _event_name(event)
            if name == "task-failed":
                _raise_task_failed(event)
            if name == "task-finished":
                return "".join(sentences[index] for index in sorted(sentences))
            if name != "result-generated":
                continue

            payload = event.get("payload")
            output = payload.get("output") if isinstance(payload, dict) else None
            sentence = output.get("sentence") if isinstance(output, dict) else None
            if not isinstance(sentence, dict):
                continue
            sentence_id = sentence.get("sentence_id", sentence.get("index"))
            text = sentence.get("text")
            sentence_end = sentence.get("sentence_end")
            heartbeat = sentence.get("heartbeat", False)
            if (
                isinstance(sentence_id, int)
                and sentence_id > 0
                and isinstance(text, str)
                and sentence_end is True
                and heartbeat is not True
            ):
                sentences[sentence_id] = text


class SpeechPipeline:
    """Factory for Fun-ASR sessions and CosyVoice audio streams."""

    def __init__(self, config: SpeechConfig, connector: Connector | None = None) -> None:
        self.config = config
        self._connector = connector or _default_connect

    def ensure_configured(self) -> None:
        if not self.config.api_key:
            raise SpeechPipelineError(
                "api_key_missing",
                "DASHSCOPE_API_KEY is not configured",
            )

    def create_asr_session(self) -> FunAsrSession:
        self.ensure_configured()
        return FunAsrSession(self)

    async def _connect(self) -> Any:
        self.ensure_configured()
        headers = {"Authorization": f"Bearer {self.config.api_key}"}
        if self.config.workspace_id:
            headers["X-DashScope-WorkSpace"] = self.config.workspace_id
        try:
            return await self._connector(
                self.config.websocket_url,
                headers,
                self.config.task_start_timeout,
            )
        except SpeechPipelineError:
            raise
        except Exception as exc:
            raise SpeechPipelineError("websocket_connect_failed", str(exc)) from exc

    async def _wait_for_started(self, websocket: Any) -> None:
        try:
            while True:
                message = await asyncio.wait_for(
                    websocket.recv(),
                    timeout=self.config.task_start_timeout,
                )
                event = _event(message)
                name = _event_name(event)
                if name == "task-failed":
                    _raise_task_failed(event)
                if name == "task-started":
                    return
        except TimeoutError as exc:
            raise SpeechPipelineError("task_start_timeout", "Speech task start timed out") from exc

    async def synthesize(self, text: str) -> AsyncIterator[bytes]:
        """Yield raw mono PCM16 bytes without modifying *text*."""

        self.ensure_configured()
        if text == "":
            return

        task_id = str(uuid.uuid4())
        websocket = await self._connect()
        async with websocket:
            await websocket.send(
                json.dumps(
                    {
                        "header": {
                            "action": "run-task",
                            "task_id": task_id,
                            "streaming": "duplex",
                        },
                        "payload": {
                            "task_group": "audio",
                            "task": "tts",
                            "function": "SpeechSynthesizer",
                            "model": self.config.tts_model,
                            "parameters": {
                                "text_type": "PlainText",
                                "voice": self.config.tts_voice,
                                "format": "pcm",
                                "sample_rate": self.config.sample_rate,
                                "volume": 50,
                                "rate": 1.0,
                                "pitch": 1.0,
                            },
                            "input": {},
                        },
                    },
                    ensure_ascii=False,
                )
            )
            await self._wait_for_started(websocket)
            await websocket.send(
                json.dumps(
                    {
                        "header": {
                            "action": "continue-task",
                            "task_id": task_id,
                            "streaming": "duplex",
                        },
                        "payload": {"input": {"text": text}},
                    },
                    ensure_ascii=False,
                )
            )
            await websocket.send(
                json.dumps(
                    {
                        "header": {
                            "action": "finish-task",
                            "task_id": task_id,
                            "streaming": "duplex",
                        },
                        "payload": {"input": {}},
                    }
                )
            )

            while True:
                try:
                    message = await asyncio.wait_for(
                        websocket.recv(),
                        timeout=self.config.task_finish_timeout,
                    )
                except TimeoutError as exc:
                    raise SpeechPipelineError("tts_timeout", "TTS task timed out") from exc
                if isinstance(message, bytes):
                    if message:
                        yield message
                    continue
                event = _event(message)
                name = _event_name(event)
                if name == "task-failed":
                    _raise_task_failed(event)
                if name == "task-finished":
                    return
