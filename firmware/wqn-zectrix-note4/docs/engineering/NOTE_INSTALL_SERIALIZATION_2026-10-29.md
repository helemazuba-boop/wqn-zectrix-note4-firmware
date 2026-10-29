# R-01: serialize full and targeted Note installations

Baseline: `f17151c` (the R-28 request encoder repair is already included).

Full Note synchronization runs on the bulk cloud lane while a missing notebook
can be installed on the interactive lane. Both publish one manifest and prune
one pack namespace. Serializing individual storage writes did not protect the
interval from remote/local snapshot reads through download and publication;
an older operation could overwrite a newer installation and later prune it.

Both public installation functions now share a nonblocking atomic admission
guard. Ownership begins before any remote or local manifest read and spans
capacity checks, downloads, merge, manifest save and index rebuild. All returns
release ownership through RAII. A competing operation returns
`ESP_ERR_INVALID_STATE` with a retry message before touching network or storage;
it publishes neither index nor successful snapshot and does not signal missing
authentication. It must start with a fresh remote snapshot when retried.

This chooses the audit's shared-install-serialization alternative. It does not
invent ordering assumptions for notebook revision counters or deletion/recreation.
There is no waiting mutex on the interactive worker. A user opening a missing
pack while bulk sync owns admission may need to retry after seeing the message;
the bulk content scheduler retains its existing failure/retry path. The gate
does not preempt a stalled owner or guarantee fair admission under repeated
contention. Existing per-request network deadlines remain responsible for exit.

Run:

```sh
python3 scripts/test_note_install_serialization.py
python3 scripts/test_ui_note.py
source /home/unknow/esp/esp-idf-v5.5/export.sh
idf.py -B build-wsl build
```

The host regression compiles an unchanged copy of the production
`main/note_cloud.cpp`, real public headers and the actual manifest merge body.
Only storage/network calls are ports. It scripts both lane directions and
same-lane reentry at fetch, local read, download decision, capacity, download,
save and index boundaries. Rejected callers perform no port calls. It also
covers failures and later retry, failed-save preservation, deleted/recreated
notebooks, first install and invalid inputs. The baseline fails 112 assertions;
the repair passes all 281 with ASan, UBSan and leak detection. This supplements
the frozen audit's filesystem/manifest lost-update evidence; it does not claim
to test ESP32 task timing, SPIFFS atomicity or physical power loss.

Note UI's existing 15,639 assertions pass. ESP-IDF v5.5.6 builds with zero
warnings, M8 ownership and compiled UI stack gates passing. Device and release
acceptance remain deferred under the user's instruction. No flash, deployment,
push or PR occurs in this batch.

This repair covers the two current Note cloud installation callers, the only
callers publishing/resetting the Note manifest in current source. It does not
change disk formats, the raw storage APIs, server snapshot semantics or R-06's
existing classification/reset policy. An external future installer must use the
same admission boundary. The gate ends when the installer returns, not when its
UI mailbox is acknowledged. The subsequent [N-02 repair](NOTE_INDEX_DELIVERY_2026-10-30.md)
orders already-built indexes across lanes and pending activation without
extending admission, and shares the failed-rebuild marker.

Evidence archive: `/home/unknow/projects/doc/robustness-remediation-20261029/r01/`.
