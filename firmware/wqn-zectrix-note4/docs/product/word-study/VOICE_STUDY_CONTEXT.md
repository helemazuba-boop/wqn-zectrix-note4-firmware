# Note4 voice study context update

The Flash default instructions now describe wrong-question browsing, new Note
creation and Todo scheduling. They defer dates, timezone, notebook targets and
available tools to the cloud's current-turn context. They prohibit content
updates and voice Word operations, and only acknowledge durable tool success.

The paired WQN change reduces voice tools from eleven to eight, injects notebook
permissions and current account-local time, browses manual/smart wrong-question
collections, and records device-bound atomic write receipts. See the cloud
repository's `docs/ESP32_VOICE.md` for relative-date semantics and migration order.

Audio/SSE/WebSocket formats, firmware API paths, local Word study and storage
protocols are preserved. No power, sleep, EPD timing or storage ownership changes
are included. Provider secrets and authorization stay on the server.

Validation: `idf.py -B build-ai-local-s3 build` completed with zero compiler
warnings and `M8 architecture ownership gate passed`; CRLF-aware diff check
passed. This is build evidence, not device HIL. The device has not been flashed.

Release the matching Web and realtime-proxy images after cloud migrations,
then verify STD/PRO and Flash with a real device: tomorrow's Todo in account
local time, reminder-time clarification, direct Note creation, collection
browsing, full options, retry/reconnect and cancellation during context refresh.

Existing uncommitted serial-monitor scripts are unrelated and excluded from
this update's commit. No push or deployment is authorized by the local commit.
