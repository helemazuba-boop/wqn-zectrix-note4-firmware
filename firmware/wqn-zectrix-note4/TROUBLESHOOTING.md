# Note4 firmware troubleshooting

Start with the full boot log, reset reason, ELF SHA-256, wake context, schema
generation, display revision and sleep generation. Decode panics only with the
ELF from the same build.

## Schema reset appears to hang

The first generation-3 boot erases default NVS and formats the 8 MiB SPIFFS
partition. Formatting can take tens of seconds with no intermediate log. A
healthy sequence is:

```text
storage schema reset required ... target=3
storage SPIFFS validated ...
storage schema committed: generation=3 ... restart required
storage schema migration complete ... restarting
```

The software restart re-enumerates native USB, so a serial reader may report
COM port loss before the next ROM banner. Use the auto-reopening listener and
wait for the second boot. If the marker is absent on the second boot, preserve
the log and inspect NVS/SPIFFS errors; do not allow business services to bypass
the schema gate.

## No sleep while USB is attached

This is expected. A PC is detected from native USB Serial/JTAG SOF frames;
charger-only power is detected from the charging/full-status GPIOs. Either signal
owns the `kUsbPower` sleep lease, which blocks automatic light sleep and deep sleep
so flashing, monitoring and AI Flash are not cut off. The IDF
`CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION` guard independently blocks automatic light
sleep while a host is attached. Disconnect USB and power the board from the battery
before evaluating the 60-second idle path or measuring sleep current.

On a connected PC, the expected boot log contains `host=1` and
`light/deep sleep blocked`. Seeing `host=0 charging=0 full=0` while the COM device
is enumerated means the USB connection monitor is not active in that build.

If battery-only sleep still does not occur, inspect lease warnings, UI idle
time, token/claim state and `prepare-sleep result` for every service. A denial
or active wake pin rolls back and enforces a 30-second retry backoff.

## Immediate wake loop

- Check `WakeContext` raw cause, EXT1 mask, PCF `AF/TF`, snapshot CRC and sleep
  generation.
- PCF flags must be cleared before wake-source assembly; its interrupt pin must
  then verify inactive.
- Treat undefined wake, brownout or invalid snapshot as cold/untrusted.
- Confirm GPIO17 remains latched and USB D-/D+ (GPIO19/20) are untouched.

## Display freeze or stale frame

- Match every accepted revision with one `Presented`, `Superseded` or `Failed`
  result. Missing terminal results are a firmware defect.
- `Presented` is valid only after panel BUSY releases. A transfer error or power
  cut must not update the RTC panel cache.
- A BUSY timeout should perform one reset/reinitialize/forced-full retry. A
  second failure leaves the service in fault/untrusted state; the next attempt
  must be full.
- Check the refresh task and UI task stack high-water logs. The accepted M2
  reference was 6480 bytes at task start and 4432 bytes after state load.
- Only `display_service` may access GPIO6 or EPD SPI; the build gate will reject
  a second owner.

## Pairing or sync failure

- After Wi-Fi provisioning, confirm an 8-digit claim code is shown and claim
  polling uses protocol header 3.
- MAC/hardware ID only identifies a candidate; it cannot recover a credential.
- Only HTTP 401 may clear the stored device token. Timeout, DNS/TLS error, 429
  and 5xx retain identity and follow retry metadata.
- Repeating the same `request_id` must return the original idempotent result.
- Confirm firmware/WQN schema hashes and the exact `/api/esp32/v3/*` endpoint;
  do not diagnose against the old Supabase Cloud checkpoint.

## Audio or Flash blocks sleep

Capture, playback and Flash sessions intentionally hold leases. On cancel,
network loss or server error, verify a terminal result and lease release. I2S,
ES8311 and amplifier GPIO access must exist only in `AudioService`; the M8 build
gate enforces this.

## Word card stalls or advances incorrectly

- Match the button log to `word observation durable`. The card must remain in
  `Persisting` until the foreground storage transaction succeeds.
- Use `owner`, `queue_wait_ms` and `elapsed_ms` to distinguish queue contention
  from SPIFFS work. Word observation commits use the foreground queue; outbox
  upload/ACK must yield after the current idempotent item when interaction
  generation changes.
- A network timeout must leave the same request id in the outbox. Repeated
  observations with a new request id are a defect.
- A resumed random session must retain its original session id, seed, snapshot,
  cursor and position. If order changes after reset, preserve `wsr.v1`, the boot
  log and the matching ELF.
- A dictionary card is read-only until an explicit known/unknown/skipped action.
  A late lookup or session result must not replace the active picker/card.
- New word-pack manifests are staged for the next session. If an active card
  changes after sync, compare the persisted session snapshot against the loaded
  pack SHA and stop using that build.

## Useful local checks

```bash
cd /home/unknow/projects/firmware/firmware/wqn-zectrix-note4
cmake -DWQN_PROJECT_DIR="$PWD" -P cmake/verify_architecture.cmake
git diff --check
source /home/unknow/esp/esp-idf-v5.5/export.sh
idf.py --no-ccache -B build-ai-local-s3 build
```

For a panic, keep the full register dump/backtrace and the matching
`build-ai-local-s3/wqn-zectrix-note4.elf`; a backtrace without its exact ELF is
not actionable.
