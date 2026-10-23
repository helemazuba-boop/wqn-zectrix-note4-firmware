# Firmware documentation map

This file explains why each documentation area exists and where to start. Keep
the project entry points (`README.md`, `README.zh-CN.md`, and `AGENTS.md`) at the
firmware root. Component and contract `README.md` files stay beside the code or
contract they describe.

## Start here

- `README.md` / `README.zh-CN.md`: what the device does, how to build and flash,
  and links to the detailed references.
- `AGENTS.md`: mandatory invariants and change workflow for firmware work.
- `docs/architecture/ARCHITECTURE.md`: runtime layers, ownership, and display
  completion semantics.
- `docs/development/BUILD_HERE.md`: build, flash, and recovery procedures.
- `docs/development/TROUBLESHOOTING.md`: symptom-led diagnosis from logs and
  runtime state.
- `docs/release/RELEASE_CHECKLIST.md`: coordinated firmware/cloud release gates.
- `docs/engineering/FIRMWARE_RELIABILITY_PLAN.md`: current reliability findings,
  maintenance gaps, and the staged patch plan for `fix/firmware-reliability`.

## Architecture

These documents explain stable runtime structure and reusable lifecycle rules.

- `docs/architecture/ARCHITECTURE.md` — component boundaries, resource owners,
  startup and sleep, and enforced architecture gates.
- `docs/architecture/STUDY_RUNTIME_REUSE.md` — which study-session lifecycle
  behavior can be shared by Word, problem, and Note features.

## Development and operations

These documents help build, diagnose, and operate the firmware. They are not
product feature specifications.

- `docs/development/BUILD_HERE.md` — build profiles, flashing, monitor, recovery,
  and local workflow.
- `docs/development/DEV_DIAGNOSTICS.md` — developer menu layout and diagnostic
  data contract; referenced by code and the release gate.
- `docs/development/TROUBLESHOOTING.md` — diagnostic steps for observed device
  failures.
- `docs/development/UI_FONT_RESOURCES.md` — font and brand asset provenance,
  licenses, and regeneration notes.

## Product and Word study

These documents describe user-visible study behavior, persisted data, and the
Word feature's runtime contract.

- `docs/product/word-study/WORD_BASELINE.md` — product semantics and current
  device/server responsibility baseline.
- `docs/product/word-study/STUDY_SESSION_PROTOCOL.md` — session creation,
  ordering, observations, and resume behavior.
- `docs/product/word-study/WORD_PACK_V2.md` — deterministic Word Pack format and
  bounded resource rules.
- `docs/product/word-study/WORD_OFFLINE_OUTBOX.md` — durable offline observation
  ordering and retry invariants.
- `docs/product/word-study/WORD_RELEASE_CHECKLIST.md` — Word-specific cutover and
  device acceptance gates.
- `docs/product/word-study/WORD_UI_REWRITE.md` — implementation and validation
  record for the Word screen design.
- `docs/product/word-study/VOICE_STUDY_CONTEXT.md` — voice-study context and
  migration notes; it points to a local companion note under the ignored repo
  `docs/` directory when present.

## User interface and display

These are feature design, implementation, or regression records for the native
UI. Use the architecture and development references above for system-wide rules.

- `docs/ui/AGENT_UI_REFINEMENT.md` — Agent page layout and interaction work.
- `docs/ui/HOME_UI_REWRITE.md` — home screen presentation and standby behavior.
- `docs/ui/NOTE_UI_REFINEMENT.md` — Note list, reader, paging, and refresh work.
- `docs/ui/PROBLEM_UI_REFINEMENT.md` — problem review layouts and interaction.
- `docs/ui/SETTINGS_UI_REWRITE.md` — settings layout, feedback, and font work.
- `docs/ui/TODO_UI_REFINEMENT.md` — Todo selection, paging, and completion UI.
- `docs/ui/UI_INTERACTION_REGRESSION.md` — known input/render regressions and
  their reproduction conditions.
- `docs/ui/EPD_IDLE_SCHEDULER.md` — display idle maintenance scheduling change
  and its pending device acceptance matrix.
- `docs/ui/EPD_MINUTE_REFRESH.md` — minute-refresh investigation and rendering
  validation.

## Engineering records

These are bounded repair or maintenance records. They preserve why a change was
made and what evidence was collected; current behavior and invariants are defined
by code, `AGENTS.md`, and the architecture/product references above.

- `docs/engineering/AUDIO_RUNTIME_REPAIR.md` — audio runtime device acceptance
  procedure and evidence boundary.
- `docs/engineering/FIRMWARE_RELIABILITY_PLAN.md` — dated source/history findings,
  issue IDs, recovery-policy questions, patch batches, and acceptance criteria.
- `docs/engineering/RELIABILITY_BATCH_01.md` — first-batch implementation status,
  clean SDK/configuration baseline, focused checks, and remaining device evidence.
- `docs/engineering/RELIABILITY_BATCH_02.md` — boot-data preservation decisions,
  durable reset intent, regression evidence, and user-confirmed device acceptance.
- `docs/engineering/RELIABILITY_BATCH_03.md` — runtime outbox preservation fixes,
  consolidated fault regressions, and user-confirmed checkpoint acceptance.
- `docs/engineering/RELIABILITY_BATCH_04.md` — RTC trust, shared I2C timeout
  repairs, production-source fault regressions, and user-confirmed acceptance.
- `docs/engineering/RELIABILITY_BATCH_05.md` — wake deadlines, short hardware
  waits, timer restoration, and the failed device acceptance record.
- `docs/engineering/RELIABILITY_BATCH_05_HOTFIX.md` — ADC/UI latency regression
  evidence, targeted rollback, complete-batch tests, and user-confirmed recovery.
- `docs/engineering/RELIABILITY_BATCH_06.md` — codec whole-call budgets,
  session/drain cleanup, healthy-path cost comparison, and user-confirmed acceptance.
- `docs/engineering/RELIABILITY_BATCH_07.md` — sleep-side RTC persistence
  deadlines, uncertain STOP cleanup, time-consumer audit, and user-confirmed acceptance.
- `docs/engineering/RELIABILITY_BATCH_08.md` — clock source/epoch tracking,
  conservative timer restoration, integrated SNTP/RTC tests, and user-confirmed acceptance.
- `docs/engineering/RELIABILITY_BATCH_09.md` — SSE whole-request budgets,
  HTTP/WS overflow termination, shared voice endpoint configuration, and user-confirmed acceptance.
- `docs/engineering/RELIABILITY_BATCH_10.md` — production GPIO/task/ring replay,
  delayed PTT release preservation, burst capacity, ISR state and user-confirmed acceptance.
- `docs/engineering/RELIABILITY_BATCH_11.md` — bounded RTC fault journal,
  boot/storage provenance, torn-write recovery, resource budget and device checks.
- `docs/engineering/DEAD_CODE_CLEANUP.md` — evidence and scope for firmware
  cleanup decisions.
- `docs/engineering/SYNC_V3_REPAIR.md` — sync repair findings, host checks, and
  remaining device verification.
- `docs/engineering/WIFI_CONNECTIVITY_REPAIR.md` — provisioning and network
  switching repair record.

## Release

- `docs/release/RELEASE_CHECKLIST.md` — coordinated firmware and WQN release,
  including automated gates, device HIL, and rollback.

## Contracts and component documentation

These documents intentionally remain adjacent to their implementation or
versioned contract so the scope is unambiguous.

- `contracts/agent-gateway-v0/README.md` — Agent Gateway v0 wire contract.
- `contracts/word-study-v1/README.md` — Word Study v1 wire contract.
- `components/qr_code/README.md` — QR component usage and scope.
- `components/wqn_wifi_provision/README.md` — Wi-Fi provisioning component;
  its `assets/*.html` files are captive-portal pages served by the firmware.
- `main/ui/assets/navigation/README.md` — provenance and use of navigation UI
  assets.

## Local-only material

The repository ignore rules still cover newly created files under `docs/`,
`analysis/`, and `backups/`. The canonical `docs/` references listed above are
explicitly tracked; deliberately add new shared reports to Git as well. The
local-only material listed below holds maintainer handoffs/specifications,
investigation scratch data and logs, and device backups/validation artifacts.
It is not part of the tracked documentation set and should not be used as the
sole source for a firmware contract or release decision. Preserve useful
conclusions in a tracked reference or report before local material is retired.

- `docs/diagnostics/artifact-storage-error-manifest-500-2026-08-25.md` records
  the 2026-08-25 Note/Problem manifest 500 investigation.
- `docs/diagnostics/opencode-agent-gateway-gaps-2026-09-24.md` is a dated,
  read-only cross-repository Agent gateway gap audit.
- `docs/diagnostics/sync-liveness-audit-2026-08-19.md` is a dated sync liveness
  and scheduler evidence snapshot.
- `docs/opencode-v2-migration-complete.md` records completion of the local
  OpenCode v2 migration.
- `docs/ui-storage-write-remediation-plan.html` is a UI storage remediation
  plan, not an implementation or acceptance report. These are local context,
  not canonical firmware rules.
- `analysis/` — temporary investigation workspaces, plans, test output, and
  intermediate evidence. Directory names identify the investigation. Within
  them, `plan.before*` captures the problem and intended scope, `plan.after*`
  captures the resulting plan, `source-manifest.json` / `identity.json` record
  which source/build snapshot the evidence belongs to, `test_*.txt` and `hil*`
  are captured check output, and `final-*` / `candidate-*` hold named
  checkpoints. A `HIL-PENDING.md` means device acceptance was still outstanding
  for that checkpoint. Use the final report or manifest to understand an
  investigation's result; scratch plans and outputs are not current
  specifications.
- `backups/` — local recovery and validation snapshots. These may contain
  device-specific evidence and are not source documentation.

Source comments still point at older `docs/*.md` design notes in a few places.
Those paths are historical references and may be absent from this checkout;
use the tracked references above for current firmware behavior.
