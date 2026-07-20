# Note4 v3 coordinated release checklist

Firmware and WQN control-plane v3 are one release unit. Do not point production
traffic at a firmware/cloud pair that was not tested together. The authoritative
self-hosted database and proxy procedure is
`/home/unknow/projects/WQN/deploy/supabase-selfhost/README.md`.

## 1. Freeze the candidate

- Record the firmware and WQN commit IDs, ESP-IDF revision, `sdkconfig` profile,
  database migration head and container image digests.
- Confirm the firmware contract manifest/schema hash matches WQN fixtures.
- Confirm AI SSE and `wqn-flash-v2` wire layouts are unchanged.
- Confirm no credentials, Wi-Fi passwords, database URLs or device tokens are
  present in source, logs or release artifacts.

## 2. Automated gates

Firmware:

```bash
cd /home/unknow/projects/firmware/firmware/wqn-zectrix-note4
source /home/unknow/esp/esp-idf-v5.5/export.sh
idf.py --no-ccache -B build-ai-local-s3 build
git diff --check
```

The build must print `M8 architecture ownership gate passed`. Record binary
size and SHA-256.

WQN:

```bash
cd /home/unknow/projects/WQN/web
npm run prepush
npm run smoke:m7-cutover
```

Run realtime proxy tests separately if that image changed. Database migration
dry-run, environment validation and target verification remain mandatory even
when application tests pass.

## 3. Cloud cutover

1. Deploy the isolated `data.helema.cn` Supabase stack and apply additive
   migrations through the guarded target script.
2. Stop WQN/realtime write entry points for the maintenance window.
3. Deploy WQN and realtime images against the new target and smoke through an
   `/etc/hosts` override.
4. Require old control endpoints to return `UPGRADE_REQUIRED`; do not run a
   long-lived v2/v3 compatibility stack.
5. Verify Auth, REST, Storage, Realtime, claim/start and claim/poll before DNS
   cutover. Do not expose Studio or Kong root.

## 4. Device HIL

- Full erase/first boot repeats schema generation 3 safely and performs exactly
  one intentional restart before business services start.
- Provision Wi-Fi, display an 8-digit claim code, approve it in the authenticated
  web flow, then verify bootstrap and sync.
- Verify home/time/Todo/Word/settings, 500 mixed display intents and terminal
  results, including partial-to-full promotion and one injected BUSY timeout.
- On battery power, run 100 sleep/wake cycles and confirm no immediate re-wake,
  interrupted refresh or interrupted storage commit.
- Verify an enumerated USB host logs `host=1` and remains connected for at least
  10 minutes (including an active AI Flash session); charger-only power must also
  block sleep. Unplug USB before measuring deep sleep. Validate PCF timer and button
  wake independently.
- Run 20 network recoveries, recording that non-401 failures retain identity.
- Verify record/playback and AI/Flash cancellation, disconnect and error paths
  release their leases and buffers.
- Run 10 interrupted generation-reset/re-pair cycles if storage schema code
  changed.

## 5. Power/performance evidence

- Use the same board, supply, temperature and instrument as baseline.
- Record at least three five-minute samples and P50/P95 for active, refresh, UI
  idle, light sleep and deep sleep.
- Enforce the plan thresholds: active <= 110% baseline P95, refresh <= 105%,
  idle/sleep moves at least 20% toward hardware floor, and wake-to-presented P95
  <= 115% baseline.

## 6. Release and rollback

Release only after all automated and HIL evidence is attached to the two commit
IDs. If any cloud or device gate fails, stop new writes and roll WQN, realtime,
runtime configuration and firmware back as one checkpoint. Preserve the failed
database and logs for diagnosis; do not reverse-merge data during the incident.
Because generation 3 erases device-local state, a rolled-back device must pair
again.
