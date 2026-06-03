#!/usr/bin/env python3
"""PC-side Qwen Omni realtime audio dialogue prototype for WQN Note4.

This script is intentionally a host-side prototype. It validates DashScope
Realtime audio events, transcription, and playback before the flow is ported to
the ESP32 firmware audio stack.
"""

from __future__ import annotations

import argparse
import base64
import os
import signal
import sys
import time
from typing import Any

import dashscope
import pyaudio
from dashscope.audio.qwen_omni import (
    MultiModality,
    OmniRealtimeCallback,
    OmniRealtimeConversation,
)


DEFAULT_MODEL = "qwen3.5-omni-plus-realtime"
DEFAULT_URL = "wss://dashscope.aliyuncs.com/api-ws/v1/realtime"
DEFAULT_VOICE = "Ethan"
DEFAULT_INSTRUCTIONS = "你是个人助理小云，请用幽默风趣的方式回答用户的问题"


class RealtimeAudioCallback(OmniRealtimeCallback):
    def __init__(self, audio: pyaudio.PyAudio, output_rate: int) -> None:
        self._audio = audio
        self._output_rate = output_rate
        self.output_stream: pyaudio.Stream | None = None

    def on_open(self) -> None:
        self.output_stream = self._audio.open(
            format=pyaudio.paInt16,
            channels=1,
            rate=self._output_rate,
            output=True,
        )
        print("[state] realtime session opened")

    def on_close(self, close_status_code: int, close_msg: str) -> None:
        print(f"[state] realtime session closed: {close_status_code} {close_msg}")

    def on_error(self, message: str) -> None:
        print(f"[error] {message}", file=sys.stderr)

    def on_event(self, response: dict[str, Any]) -> None:
        event_type = response.get("type")
        if event_type == "response.audio.delta":
            delta = response.get("delta")
            if delta and self.output_stream is not None:
                self.output_stream.write(base64.b64decode(delta))
        elif event_type == "conversation.item.input_audio_transcription.completed":
            transcript = response.get("transcript", "")
            print(f"[User] {transcript}")
        elif event_type == "response.audio_transcript.done":
            transcript = response.get("transcript", "")
            print(f"[LLM] {transcript}")
        elif event_type in {"response.done", "response.audio.done"}:
            print(f"[state] {event_type}")

    def close(self) -> None:
        if self.output_stream is not None:
            self.output_stream.stop_stream()
            self.output_stream.close()
            self.output_stream = None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run a PC-side Qwen Omni realtime voice dialogue session.",
    )
    parser.add_argument("--model", default=DEFAULT_MODEL)
    parser.add_argument("--url", default=DEFAULT_URL)
    parser.add_argument("--voice", default=DEFAULT_VOICE)
    parser.add_argument("--instructions", default=DEFAULT_INSTRUCTIONS)
    parser.add_argument("--input-rate", type=int, default=16000)
    parser.add_argument("--output-rate", type=int, default=24000)
    parser.add_argument(
        "--chunk-frames",
        type=int,
        default=3200,
        help="Microphone frames per append_audio call.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    api_key = os.getenv("DASHSCOPE_API_KEY")
    if not api_key:
        print("DASHSCOPE_API_KEY is not set.", file=sys.stderr)
        return 2

    dashscope.api_key = api_key
    should_stop = False

    def handle_signal(signum: int, _frame: object) -> None:
        nonlocal should_stop
        should_stop = True
        print(f"\n[state] stopping after signal {signum}...")

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    audio = pyaudio.PyAudio()
    callback = RealtimeAudioCallback(audio, args.output_rate)
    conversation: OmniRealtimeConversation | None = None
    mic_stream: pyaudio.Stream | None = None

    try:
        conversation = OmniRealtimeConversation(
            model=args.model,
            callback=callback,
            url=args.url,
        )
        conversation.connect()
        conversation.update_session(
            output_modalities=[MultiModality.AUDIO, MultiModality.TEXT],
            voice=args.voice,
            instructions=args.instructions,
        )

        mic_stream = audio.open(
            format=pyaudio.paInt16,
            channels=1,
            rate=args.input_rate,
            input=True,
            frames_per_buffer=args.chunk_frames,
        )

        print("[state] 对话已开始，对着麦克风说话。按 Ctrl+C 退出。")
        while not should_stop:
            audio_data = mic_stream.read(
                args.chunk_frames,
                exception_on_overflow=False,
            )
            conversation.append_audio(base64.b64encode(audio_data).decode("ascii"))
            time.sleep(0.01)
    finally:
        if conversation is not None:
            conversation.close()
        if mic_stream is not None:
            mic_stream.stop_stream()
            mic_stream.close()
        callback.close()
        audio.terminate()
        print("[state] 对话结束")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
