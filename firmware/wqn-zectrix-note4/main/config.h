#pragma once

#define WQN_FIRMWARE_NAME "wqn-zectrix-note4"
#define WQN_FIRMWARE_VERSION "0.1.0"
#define WQN_BOARD_ID "zectrix-s3-epaper-4.2"

#ifndef WQN_API_BASE
#define WQN_API_BASE "https://wqn.helema.cn/api/esp32"
#endif

// Derived from WQN_API_BASE: used as the WebSocket origin / host. Today we
// only ship one deployment, so the literal is fine; if we ever support
// multiple environments, we will compute this from WQN_API_BASE at startup.
#ifndef WQN_API_BASE_HOST
#define WQN_API_BASE_HOST "wqn.helema.cn"
#endif

#define WQN_NVS_NAMESPACE "wqn"
#define WQN_NVS_ACCESS_TOKEN_KEY "access_token"

#define WQN_SYNC_LIMIT 20

// ============================================================================
// AI v2 SSE streaming protocol (STD/PRO tier) — see docs/04-std-pro-streaming-protocol.md
// ============================================================================
#ifndef WQN_AI_SSE_REQUEST_PATH
#define WQN_AI_SSE_REQUEST_PATH "/ai/transcribe-chat"
#endif

#ifndef WQN_AI_SSE_HEADER_PROTOCOL
#define WQN_AI_SSE_HEADER_PROTOCOL "X-WQN-Protocol"
#endif
#ifndef WQN_AI_SSE_PROTOCOL_VALUE
#define WQN_AI_SSE_PROTOCOL_VALUE "v2-streaming"
#endif
#ifndef WQN_AI_SSE_HEADER_ACCEPT
#define WQN_AI_SSE_HEADER_ACCEPT "X-WQN-Accept"
#endif
#ifndef WQN_AI_SSE_ACCEPT_VALUE
#define WQN_AI_SSE_ACCEPT_VALUE "text/event-stream"
#endif

// SSE stream must finish before this many milliseconds; we keep the connect
// generous because server-side ASR/LLM may run for up to 360s.
#ifndef WQN_AI_SSE_TIMEOUT_MS
#define WQN_AI_SSE_TIMEOUT_MS (10 * 60 * 1000)
#endif

// SSE chunk render throttle — see page_ai.cpp. Partial fragments get coalesced
// so the EPD does not partial-refresh faster than it can physically finish one.
#ifndef WQN_AI_SSE_CHUNK_RENDER_INTERVAL_MS
#define WQN_AI_SSE_CHUNK_RENDER_INTERVAL_MS 250
#endif

// ============================================================================
// Flash Realtime v2 protocol — see docs/07-flash-realtime-protocol.md
// ============================================================================
#ifndef WQN_FLASH_WS_PATH
#define WQN_FLASH_WS_PATH "/api/esp32/realtime"
#endif
#ifndef WQN_FLASH_WS_MODEL
#define WQN_FLASH_WS_MODEL "wqn-flash-v2"
#endif
#ifndef WQN_FLASH_WS_SUBPROTOCOL
#define WQN_FLASH_WS_SUBPROTOCOL "wqn-flash-v2"
#endif

#ifndef WQN_FLASH_VOICE
#define WQN_FLASH_VOICE "qingchunshaonv"
#endif

#ifndef WQN_FLASH_DEFAULT_INSTRUCTIONS
#define WQN_FLASH_DEFAULT_INSTRUCTIONS "你是WQN中的学习助手，使用简要的中文回答用户问题"
#endif

// Output sample rate is negotiated at session.update time; StepAudio Realtime
// is native 24 kHz. The I2S duplex path also runs at 24 kHz (shared BCLK/WS
// with the microphone capture), so no firmware-side resampler is needed -
// the proxy forwards 24 kHz PCM verbatim in both directions.
#ifndef WQN_FLASH_OUTPUT_SAMPLE_RATE_HZ
#define WQN_FLASH_OUTPUT_SAMPLE_RATE_HZ 24000
#endif

// Upstream audio chunk (720 B = 360 frames @ 24 kHz = 15 ms).
#ifndef WQN_FLASH_AUDIO_CHUNK_FRAMES
#define WQN_FLASH_AUDIO_CHUNK_FRAMES 360
#endif
#ifndef WQN_FLASH_AUDIO_CHUNK_BYTES
#define WQN_FLASH_AUDIO_CHUNK_BYTES (WQN_FLASH_AUDIO_CHUNK_FRAMES * 2)
#endif
