# N-02: order Note index delivery across cloud lanes

Baseline: firmware `fa12f6f`. R-01 serializes full/targeted manifest installation
and index construction. Its admission ends before UI mailbox acknowledgement,
so completed indexes can still arrive out of build order. Mailbox generations
identify results within a domain, not between interactive and bulk domains.

Actual UI apply paths reproduce two failures. An older full index can remain
staged after a newer targeted index was installed, then overwrite it on return
to the notebook list. An older late delivery can also overwrite an active or
pending index. The normal UI scan visits interactive Note before bulk Note,
permitting a new interactive result followed by an already-pending older bulk
result. These are RAM index/offset regressions; no persistent rollback or device
incident is inferred, and the R-01 manifest repair remains intact.

Both installers reserve a shared 64-bit process-local ordinal under the existing
admission guard, before I/O. A successfully returned index carries that ordinal
in `NotePackIndex::delivery_generation`. Failed/no-index results carry zero;
failed attempts may leave ordinal gaps. Exhaustion saturates and returns an
error before I/O, preventing wrap. Initial local loads use zero. No remote
cursor/revision order, persisted field, schema or wire change is introduced.
Reboot resets the ordinal with the RAM indexes/mailboxes. Nonblocking admission
and its original lifetime remain intact.

Full-index apply rejects generations older than the active or pending index.
Installation rejects older active replacements and retires pending data only
when superseded. A newer full index stays staged during the current browse:
targeted completion can honor its body-open intent with its own admitted index,
then returning to the notebook list activates the newer catalog. Defensive
activation cannot regress an already newer active index. Existing notebook,
selection and persist-state checks on body completion remain in place; no new
server session or pinned-content protocol is added.

Producer tests also confirm a related retry gap: a targeted install may commit
its manifest then fail index construction. The old dirty marker belonged only
to full sync, so the next unchanged full sync could skip the required rebuild.
Both installers now share that marker. Successful targeted download/publication
arms it, a failed rebuild leaves it armed, and a successful rebuild clears it.
An unchanged full sync can therefore repair and publish a new ordered index.
This covers returned rebuild failures; existing `ESP_OK`/index-status
classification and general storage recovery remain separate.

From the firmware application directory:

```sh
python3 scripts/test_note_index_delivery.py
python3 scripts/test_note_index_delivery.py --before fa12f6f
python3 scripts/test_note_index_delivery.py --mutation keep-pending
python3 scripts/test_note_install_serialization.py
python3 scripts/test_ui_note.py --sanitize
python3 scripts/test_manifest_load_preservation.py
python3 scripts/test_pack_json_boundaries.py
source /home/unknow/esp/esp-idf-v5.5/export.sh
idf.py -B build-wsl build
```

The dedicated harness compiles complete actual `note_cloud.cpp`, real model
headers, manifest merge, Note app apply/install/activation functions, pack/body
branches of UI result apply and actual mailbox functions. Network, storage/index
construction, body read, observation, home summary and notification are ports.
The two-domain host map models Note's lanes, not the complete runner/FreeRTOS.
It schedules both producer directions, both delivery orders and list/browse
states, checking ACK retirement. It also covers body-open intent, other-notebook
intent retention, deletion/recreation at the same cursor, lower remote revisions,
no-change results, initial zero generation, failed rebuild/retry and a fault-
injected obsolete pending index. Counter injection checks values above 32 bits
and saturation. No production hook or public setter is added.

The repair passes 124 ASan/UBSan/LSan assertions. Baseline passes 97 and fails
23, with 120 total because four wide/exhaustion checks need the new field.
Seven semantic mutations are rejected: omit stamps, accept older bulk, accept
older active replacement, retain superseded pending data, bypass activation,
wrap the counter and omit the targeted shared dirty marker. Compile failures
are not counted as effective controls.

Regressions pass: R-01 installer interleaving 283, Note native UI 15,639,
manifest-load preservation 87 and pack reader/index boundaries 189 assertions.
Full ESP-IDF v5.5.6 build has zero warnings and passing M8/compiled UI-stack
gates. The UI adapter includes the private helpers; normal body open, selection,
commit gating and cross-notebook intent checks remain covered. The dedicated
body port observes aggregate selection; physical SPIFFS reads, EPD, scheduler
timing, stack high-water marks and provider behavior remain device acceptance.

The ordinal does not certify server snapshot freshness, all storage recovery,
fair admission or preemption of a stalled owner. Full-sync error/status metadata
keeps its operation semantics; materialized indexes are ordered here. Device/
release acceptance remains deferred under the staged repair instruction. Logs
and source/SDK/binary identities are archived under
`/home/unknow/projects/doc/robustness-remediation-20261029/n02/`.
No model call, cloud change, push, PR, deployment or flash occurs.
