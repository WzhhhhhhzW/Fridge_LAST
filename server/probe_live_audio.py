#!/usr/bin/env python3
"""Run a short AUD1 protocol probe against the deployed speech server."""

from __future__ import annotations

import argparse
import re
import socket
import time
import wave
import zlib
from pathlib import Path


SERVER = ("101.245.87.7", 10086)
FRAME_SAMPLES = 1600
FRAME_BYTES = FRAME_SAMPLES * 2
FRAME_COUNT = 10
TOKEN_HEADER = (
    Path(__file__).resolve().parent.parent
    / "Fridge"
    / "project_firdge"
    / "src"
    / "audio_voice_config.h"
)


def configured_token() -> str:
    text = TOKEN_HEADER.read_text(encoding="utf-8")
    match = re.search(
        r'^\s*#define\s+AUDIO_VOICE_DEVICE_TOKEN\s+"([^"]+)"',
        text,
        flags=re.MULTILINE,
    )
    if match is None or not match.group(1):
        raise RuntimeError("AUDIO_VOICE_DEVICE_TOKEN is not configured")
    return match.group(1)


def read_line(stream: object) -> str:
    raw = stream.readline(512)  # type: ignore[attr-defined]
    if not raw:
        raise ConnectionError("server closed the connection")
    return raw.decode("ascii", errors="replace").rstrip("\r\n")


def load_pcm(wav_path: str | None) -> bytes:
    if wav_path is None:
        return bytes(FRAME_BYTES * FRAME_COUNT)

    with wave.open(wav_path, "rb") as source:
        if (
            source.getnchannels() != 1
            or source.getsampwidth() != 2
            or source.getframerate() != 16000
            or source.getcomptype() != "NONE"
        ):
            raise RuntimeError("WAV must be 16 kHz mono PCM16")
        return source.readframes(source.getnframes())


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--wav", help="optional 16 kHz mono PCM16 WAV")
    args = parser.parse_args()

    token = configured_token()
    session_id = f"PC-PROBE-{int(time.time())}"
    pcm = load_pcm(args.wav)
    frames = [pcm[offset : offset + FRAME_BYTES]
              for offset in range(0, len(pcm), FRAME_BYTES)]

    with socket.create_connection(SERVER, timeout=10) as connection:
        connection.settimeout(45)
        stream = connection.makefile("rb")
        connection.sendall(
            f"AUD1 START {session_id} 16000 1 S16LE {token}\n".encode("ascii")
        )
        ready = read_line(stream)
        print(f"START_RESPONSE={ready}")
        if ready != f"AUD1 READY {session_id}":
            return

        for sequence, frame in enumerate(frames):
            checksum = zlib.crc32(frame) & 0xFFFFFFFF
            header = (
                f"AUD1 DATA {session_id} {sequence} "
                f"{len(frame) // 2} {checksum:08X}\n"
            ).encode("ascii")
            connection.sendall(header + frame)
            time.sleep(0.1)

        connection.sendall(
            f"AUD1 END {session_id} {len(frames)} "
            f"{len(pcm) // 2}\n".encode("ascii")
        )

        while True:
            header = read_line(stream)
            print(f"SERVER_HEADER={header}")
            fields = header.split(" ")
            if len(fields) >= 5 and fields[:2] == ["AUD1", "ERROR"]:
                message_size = int(fields[4])
                message = stream.read(message_size)
                print(f"SERVER_ERROR={message.decode('utf-8', errors='replace')}")
                return
            if len(fields) == 4 and fields[:2] == ["AUD1", "TEXT"]:
                text_size = int(fields[3])
                transcript = stream.read(text_size)
                print(f"TEXT_BYTES={len(transcript)}")
                print(f"TRANSCRIPT={transcript.decode('utf-8', errors='replace')}")
                continue
            if len(fields) == 6 and fields[:2] == ["AUD1", "PCM"]:
                pcm_size = int(fields[4])
                received = stream.read(pcm_size)
                if len(received) != pcm_size:
                    raise ConnectionError("short PCM response")
                continue
            if len(fields) == 5 and fields[:2] == ["AUD1", "DONE"]:
                return
            raise RuntimeError(f"unexpected server header: {header}")


if __name__ == "__main__":
    main()
