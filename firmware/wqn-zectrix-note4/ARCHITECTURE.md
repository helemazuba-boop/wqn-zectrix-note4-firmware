# Note4 firmware architecture

This document describes the M8 runtime boundaries. Code that changes a hardware
owner or reverses a dependency below must update both this document and
`cmake/verify_architecture.cmake`.

## Dependency direction

```text
features (UI / Todo / Word / AI)
                 |
                 v
service interfaces in main/services
                 |
                 v
display / power / connectivity / storage / audio / sync
                 |
                 v
runtime primitives
                 |
                 v
platform_note4 / ESP-IDF drivers
```

The firmware remains one product application, but four stable, acyclic IDF
components have been extracted:

| Component | Responsibility | Dependencies |
| --- | --- | --- |
| `platform_note4` | Earliest safe-pin state and Note4 pin constants | ESP-IDF driver |
| `power_runtime` | `SleepLease`, sleep snapshot, `WakeContext`, prepare protocol | FreeRTOS/ESP PM only |
| `device_protocol` | v3 JSON contract and claim cryptography | JSON/mbedTLS |
| `display_service` | EPD framebuffer, SPI, BUSY, GPIO6, refresh/power state | `power_runtime` |

The high-level `PowerCoordinator` stays in `main`: it coordinates service
interfaces and would create a dependency cycle if placed in `power_runtime`.
Features also remain in `main` until their interfaces are equally stable.

## Resource ownership

| Resource | Runtime owner | Client interface |
| --- | --- | --- |
| EPD framebuffer, SPI, BUSY, GPIO6 | `DisplayService` | drawing API, `DisplayIntent`, `DisplayResult`, prepare/rollback |
| Deep sleep and global sleep generation | `PowerCoordinator` | `SleepLease`, activity notification |
| Wake-source assembly and PCF8563 flag clearing | `WakeController` | called only during coordinator prepare |
| PCF8563 wall-clock persistence and trusted restore | `RtcTimekeep` (`main/power/`) | persist during coordinator prepare; restore at boot before clock consumers start |
| Wi-Fi station, provisioning and reconnect policy | `ConnectivityService` | connectivity state and capability requests |
| NVS/SPIFFS writes | `StorageService` | serialized transaction; immutable capacity snapshot for readers |
| I2S, ES8311 and amplifier GPIO | `AudioService` | fixed command/result interfaces |
| Application state and page transitions | `UiRuntime` | fixed-size events, reducer and effects |
| v3 claim/bootstrap/sync lifecycle | `SyncService` | explicit reasons, RTC-retained schedule/retry, outbox triggers, immutable snapshot and domain events |

`platform_note4` may drive rails low before services start. This is a
bootstrap-only safety exception; after bring-up it does not run again.

Storage media are deliberately tiered. NVS is reserved for bounded control
state such as identity, Wi-Fi, schema markers, revisions, cursors and settings.
SPIFFS stores reconstructable content, study packs and durable outboxes. The
problem-study-v1 path persists a manifest plus per-problem-set WQNP packs,
builds a fixed-size catalog index in PSRAM, and reads problem bodies from their
pack on demand. Problem sets appear as `[题]` rows in the Note list; review
observations enter the durable problem outbox before `/v3/problems/observations`
upload. PSRAM is never a persistence or sync-checkpoint source.

## Startup and sleep

`app_main` performs only assembly:

1. establish safe pins and shared power hardware;
2. collect `WakeContext` and enable the validated PM profile;
3. enforce storage schema generation 3 before starting business services;
4. initialize storage and service tasks;
5. admit connectivity only when the wake has network work, then start UI,
   sync and `PowerCoordinator` (interactive/cold boots retain normal
   connectivity; background timer wakes may keep Wi-Fi and EPD off).

Normal sleep is a two-phase transaction. With no active lease and after the
idle threshold, `PowerCoordinator` closes lease admission, sends a generation
and deadline to every service, prepares the wake line, writes the CRC-protected
snapshot, then calls the sole `esp_deep_sleep_start()` site. Any denial or
timeout rolls all prepared services back and retries no earlier than 30 seconds.
Wake-source assembly takes the earliest of the display-clock deadline and the
sync scheduler's periodic/retry/outbox/content deadline. The sleep snapshot
records whether that timer belongs to display or background sync, so a
sync-only wake does not initialize or refresh the panel unless new visible
content is actually accepted later in the boot.

USB/charger presence owns a sleep lease. PC attachment is detected from USB
Serial/JTAG SOF traffic instead of charger-status GPIO; active-low `CHRG_L` is
the fallback while a charger-only source is actively charging. `/STDBY` remains
a charge-complete status but is not standalone proof of external power because
Note4 hardware can keep it low after cable removal. Therefore a confirmed USB
connection prevents both automatic light sleep and deep sleep while flashing,
monitoring or using AI Flash. Battery-only HIL is required to validate sleep
current. Wake-source assembly likewise omits `/STDBY` only when it is already
low at arm time; EXT1 remains `ANY_LOW`, and `CHRG_L` remains armed to detect a
charger insertion.

The idle transaction rechecks confirmed external power after wake-source
assembly and once more in the final commit critical section. A source arriving
while service admission is closed therefore rolls preparation back instead of
depending on a USB lease that cannot be reacquired until rollback completes.
Battery percentage and depletion protection use the same confirmed-power bit;
an unpowered residual `/STDBY` remains diagnostic data but cannot force 100% or
veto the low-voltage shutdown debounce.

## Display terminal semantics

The UI owns two fixed frame slots; `DisplayService` owns the physical panel.
Every accepted revision produces exactly one terminal result:

- `Presented`: BUSY completed and the panel cache may be committed;
- `Superseded`: a newer revision replaced it before presentation;
- `Failed`: transfer, timeout or recovery failed.

At most one refresh and one merged pending intent exist. Dirty regions are
unioned, the earliest deadline wins, reason masks are ORed, and waveform
requirements only strengthen. A BUSY timeout permits one reset/reinitialize/
forced-full retry. Cold or untrusted wake context forces a full refresh. Tiny
clock-region partials may accumulate to 240; ten heavy partials still promote
to full, and idle maintenance cleans heavy history before cutting the rail.

## Enforced boundaries

Every build runs `cmake/verify_architecture.cmake`. It rejects:

- driver/HAL includes in feature sources;
- any second deep-sleep call site;
- Wi-Fi driver calls outside the connectivity adapters;
- EPD SPI/GPIO6 access outside `display_service` and safe-pin bootstrap;
- I2S/codec/amplifier access outside `AudioService` and safe-pin bootstrap;
- NVS writes outside `storage.cpp` and the generation-3 boot schema gate;
- reintroduction of the removed flattened problem cache/API/UI prototype;
- reintroduction of removed pre-M8 implementation paths.

The gate is an architectural test, not a replacement for the build, contract
fixtures, or hardware fault matrix.
