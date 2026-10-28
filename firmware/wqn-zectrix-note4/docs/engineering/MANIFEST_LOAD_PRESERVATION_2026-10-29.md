# R-06: preserve caches when manifest loading fails

Baseline: `01388a4`, including R-28 and the Note installation admission repair.

Word, Note and Problem sync previously called their destructive cache reset
after an unclassified local manifest error. Parser allocation failure can be
reported as `ESP_ERR_INVALID_RESPONSE`, so that code is not proof of corruption.
An I/O or memory failure could erase otherwise recoverable offline files.

The three sync paths now propagate every failed load except confirmed absence.
They keep the failure code, attach a status message and stop before remote
sync/download/publication. They do not reset the cache. Existing primary/backup
fallback remains in the loader; `ESP_ERR_NOT_FOUND` can reach the first-install
path only when the loader has confirmed both manifest copies missing. That path
starts from an empty in-memory manifest without deleting pack files. Normal
successful install/pruning rules still apply after a complete verified sync.

Word's load handling is also nested under successful `BeginContentInstall`.
A prerequisite `NOT_FOUND` error can no longer be misread as an absent manifest
and overwritten with a successful first-install result.

This is conservative admission, not a new parser error taxonomy. A syntactically
damaged primary and backup now block automatic sync too: retry may recover a
temporary failure, but truly damaged manifests need backup restoration or a
separate recovery flow. This batch deliberately retains ambiguous originals;
it does not claim to implement audit R-08/R-18's repair UI or general C++ OOM
recovery. No disk format, outbox identity, token rule or reset API is changed.

Run:

```sh
python3 scripts/test_manifest_load_preservation.py
python3 scripts/test_note_install_serialization.py
python3 scripts/test_pack_prune.py
source /home/unknow/esp/esp-idf-v5.5/export.sh
idf.py -B build-wsl build
```

The new test executes the actual pre-sync admission blocks extracted from all
three production callers with real public headers. Host ports track cache
preservation, domain load/reset calls and permission to continue. It injects
resource, I/O and ambiguous parse failures, checks valid/confirmed-missing
first-install paths, and preserves failed installation prerequisites. It also
executes the real Note disk decoder with real SDK cJSON allocation failure,
then retries the same valid JSON after allocation recovers. This covers 87
checks under ASan, UBSan and leak detection. Network/storage are host ports;
it does not simulate the whole filesystem or ESP32 scheduling.

Existing Note install serialization passes 281 checks. The unchanged
Word/Note/Problem publish/rollback-pruning suite passes 32,238 checks including
740 single-allocation encoder faults. ESP-IDF v5.5.6 builds with zero warnings,
M8 ownership and compiled UI stack gates passing. HIL, physical power loss and
production release acceptance remain deferred; this batch performs no flash,
deployment, push or PR.

Evidence archive: `/home/unknow/projects/doc/robustness-remediation-20261029/r06/`.
