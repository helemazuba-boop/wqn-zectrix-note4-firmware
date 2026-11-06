// Function-level host fixture for the deck-scope count cache and the
// default-deck auto-repair state machine. Production functions are injected
// verbatim (see test_word_scope_autorepair.py); everything else is a minimal
// mock. No real NVS / SPIFFS / StorageService / persist worker is touched.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr const char* kTag = "scope-autorepair-fixture";
const char* esp_err_to_name(esp_err_t) { return "injected-error"; }
void FixtureLog(const char*, const char*, ...) {}
#define ESP_LOGW(...) FixtureLog(__VA_ARGS__)
#define ESP_LOGI(...) FixtureLog(__VA_ARGS__)

namespace wqn {
bool scope_ready=true;
bool IsWordDeckScopeReady() { return scope_ready; }

// ---- Minimal mocks of the PODs the extracted functions touch -------------
struct WordDeckInfo {
    std::string deck_id;
    std::string title;
    size_t entry_count = 0;
};

struct WordPackIndexEntry {
    char word_id[37];
    char deck_id[37];
    char word[81];
    char pack_stem[28];
    uint32_t file_offset;
    uint32_t deck_order;
    int32_t sort_index;
};

struct WordPackIndex {
    std::vector<WordPackIndexEntry> entries;  // plain vector; POD allocator mocked away
};

struct DefaultDeckRepairState {
    bool pending = false;
    uint32_t op_id = 0;
    int64_t retry_after_ms = 0;
    uint32_t attempts = 0;
    uint32_t scope_generation = 0;
    bool must_complete = false;  // accepted-once recovery obligation
};

struct DefaultDeckRepairGates {
    bool observations_pending = false;
    bool persisting = false;
    bool manual_switch_pending = false;
    bool scope_reset_pending = false;
    uint32_t current_scope_generation = 0;
    int64_t now_ms = 0;
};

enum class DefaultDeckRepairTick { kIdle, kDefer, kSubmit };

struct WordAppState {
    std::string default_deck_id;
    std::string default_deck_title;
    std::string scoped_deck_id;
    std::vector<WordDeckInfo> deck_catalog;
    bool default_deck_manual_pending=false;
    bool default_deck_missing = false;
    DefaultDeckRepairState default_deck_repair;

    WordPackIndex pack_index;
    uint32_t index_generation = 1;
    mutable std::string scoped_count_scope_id;
    mutable size_t scoped_count_index_size = 0;
    mutable uint32_t scoped_count_index_generation = 0;
    mutable size_t scoped_count = 0;
};

// ---- Production: InstallWordDeckCatalog + the auto-repair state machine ---
@@WQN@@

}  // namespace wqn

// ---- Production: global scope-count helpers (take wqn::WordAppState&) ----
@@COUNT@@

// ---------------------------------------------------------------------------
static int g_pass = 0;
static int g_fail = 0;
static void Check(bool ok, const char* what)
{
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        std::printf("FAIL: %s\n", what);
    }
}

static wqn::WordPackIndexEntry MakeEntry(const char* deck_id)
{
    wqn::WordPackIndexEntry e{};
    std::strncpy(e.deck_id, deck_id, sizeof(e.deck_id) - 1);
    return e;
}

static wqn::DefaultDeckRepairGates CleanGates(uint32_t gen, int64_t now)
{
    wqn::DefaultDeckRepairGates g;
    g.current_scope_generation = gen;
    g.now_ms = now;
    return g;
}

int main()
{
    using wqn::DefaultDeckRepairTick;
    using wqn::WordAppState;

    // -------------------------------------------------------------------
    // A. Scope-aware count cache: scope switch + same-size index change.
    // -------------------------------------------------------------------
    {
        WordAppState s;
        s.pack_index.entries = {
            MakeEntry("d1"), MakeEntry("d1"), MakeEntry("d2"),
            MakeEntry("d3"), MakeEntry("d3")};  // total 5
        // All decks (no default / no scoped override).
        Check(GetScopedCount(s) == 5, "A1 all-decks count = 5");
        const size_t cached_all = s.scoped_count;
        Check(cached_all == 5, "A2 cache populated");
        // Cache hit: a second call does not rescan (memoized value retained).
        Check(GetScopedCount(s) == 5, "A3 cache hit returns 5");
        // Default deck -> scope change invalidates the cache (id changed).
        s.default_deck_id = "d2";
        Check(GetScopedCount(s) == 1, "A4 default d2 count = 1");
        Check(s.scoped_count_scope_id == "d2", "A5 cache keyed by effective scope");
        // Scoped override wins over default.
        s.scoped_deck_id = "d1";
        Check(GetScopedCount(s) == 2, "A6 scoped d1 wins = 2");
        Check(s.scoped_count_scope_id == "d1", "A7 cache keyed by scoped id");
        // Clear override -> back to default d2 = 1.
        s.scoped_deck_id.clear();
        Check(GetScopedCount(s) == 1, "A8 clears override back to default");
        // SAME-SIZE index content change: bump generation (InstallWordPackIndex
        // does this) and swap one entry's deck. Size is unchanged, but the
        // generation bump MUST invalidate the memo.
        const size_t before_size = s.pack_index.entries.size();
        s.pack_index.entries[2] = MakeEntry("d1");  // d2 -> d1
        ++s.index_generation;                       // same size, new content
        Check(s.pack_index.entries.size() == before_size, "A9 index size unchanged");
        Check(GetScopedCount(s) == 0, "A10 same-size gen bump recomputes (default d2 now 0)");
        Check(s.scoped_count_index_generation == s.index_generation,
              "A11 cache generation refreshed");
    }

    // -------------------------------------------------------------------
    // B. InstallWordDeckCatalog authoritative semantics (gap 1).
    // -------------------------------------------------------------------
    {
        WordAppState s;
        s.default_deck_id = "gone";
        // Boot / unloaded catalog: never evidence of a deletion.
        wqn::InstallWordDeckCatalog(&s, {}, /*authoritative=*/false);
        Check(!s.default_deck_missing, "B1 boot empty catalog does not arm");
        std::vector<wqn::WordDeckInfo> local = {{"other", "Other", 3}};
        wqn::InstallWordDeckCatalog(&s, local, /*authoritative=*/false);
        Check(!s.default_deck_missing, "B2 boot non-empty missing default does not arm");
        Check(s.default_deck_title.empty(), "B3 boot leaves title unresolved");

        // Authoritative EMPTY catalog = all decks deleted -> arm (gap 1).
        wqn::InstallWordDeckCatalog(&s, {}, /*authoritative=*/true);
        Check(s.default_deck_missing, "B4 authoritative empty catalog arms repair");

        // Authoritative catalog containing the default -> no arm, title resolves.
        std::vector<wqn::WordDeckInfo> present = {{"gone", "Still Here", 7}};
        wqn::InstallWordDeckCatalog(&s, present, /*authoritative=*/true);
        Check(!s.default_deck_missing, "B5 default present clears signal");
        Check(s.default_deck_title == "Still Here", "B6 title resolved");

        // Authoritative catalog missing the default -> arm.
        std::vector<wqn::WordDeckInfo> missing = {{"a", "A", 1}, {"b", "B", 2}};
        wqn::InstallWordDeckCatalog(&s, missing, /*authoritative=*/true);
        Check(s.default_deck_missing, "B7 authoritative missing default arms");
        Check(s.default_deck_id == "gone", "B8 NVS-backed default NOT cleared in memory");

        // All-decks already selected -> nothing to repair.
        s.default_deck_id.clear();
        wqn::InstallWordDeckCatalog(&s, missing, /*authoritative=*/true);
        Check(!s.default_deck_missing, "B9 all-decks needs no repair");
    }

    // -------------------------------------------------------------------
    // C. Real auto intent lifecycle: arm -> pump -> submit -> ACK (gap 5).
    // -------------------------------------------------------------------
    {
        WordAppState s;
        s.default_deck_id = "gone";
        s.default_deck_missing = true;
        Check(wqn::ArmDefaultDeckRepair(&s, /*gen=*/10), "C1 arm returns true");
        Check(s.default_deck_repair.pending, "C2 pending armed");
        Check(s.default_deck_repair.scope_generation == 10, "C3 epoch captured");
        Check(wqn::DefaultDeckRepairBlocking(s), "C4 pending blocks session start");
        Check(wqn::TickDefaultDeckRepair(&s, CleanGates(10, 0)) ==
                  DefaultDeckRepairTick::kSubmit,
              "C5 clean pump ticks kSubmit");
        wqn::OnDefaultDeckRepairSubmitted(&s, /*op=*/77, /*now=*/0);
        Check(s.default_deck_repair.op_id == 77, "C6 op attributed");
        Check(s.default_deck_repair.pending, "C7 pending stays until ACK");
        Check(wqn::TickDefaultDeckRepair(&s, CleanGates(10, 0)) ==
                  DefaultDeckRepairTick::kDefer,
              "C8 in-flight pump defers");
        Check(wqn::OnDefaultDeckRepairAck(&s, 77, ESP_OK, 0), "C9 ACK is ours");
        Check(!s.default_deck_repair.pending && s.default_deck_repair.op_id == 0 &&
                  s.default_deck_repair.attempts == 0,
              "C10 success clears the whole intent");
        Check(!wqn::DefaultDeckRepairBlocking(s), "C11 no longer blocking");
    }

    // -------------------------------------------------------------------
    // D. Failure / busy / retry with bounded backoff (gap 2/5).
    // -------------------------------------------------------------------
    {
        WordAppState s;
        s.default_deck_id = "gone";
        s.default_deck_missing = true;
        wqn::ArmDefaultDeckRepair(&s, 10);
        // Busy reserve (op_id == 0): intent preserved, backoff armed.
        wqn::OnDefaultDeckRepairSubmitted(&s, 0, /*now=*/1000);
        Check(s.default_deck_repair.pending, "D1 busy submit keeps intent");
        Check(s.default_deck_repair.retry_after_ms > 1000, "D2 backoff deadline set");
        Check(s.default_deck_repair.attempts == 1, "D3 attempt counted");
        Check(s.default_deck_repair.op_id == 0, "D4 no false attribution");
        Check(wqn::TickDefaultDeckRepair(&s, CleanGates(10, 1000)) ==
                  DefaultDeckRepairTick::kDefer,
              "D5 backoff defers");
        Check(wqn::TickDefaultDeckRepair(&s, CleanGates(10, 5000)) ==
                  DefaultDeckRepairTick::kSubmit,
              "D6 past backoff ticks kSubmit");
        wqn::OnDefaultDeckRepairSubmitted(&s, 88, 5000);
        // Durable transaction failure: re-arm pending + backoff, clear op.
        Check(wqn::OnDefaultDeckRepairAck(&s, 88, ESP_FAIL, 5001),
              "D7 failure ACK is ours");
        Check(s.default_deck_repair.pending, "D8 failure preserves intent");
        Check(s.default_deck_repair.op_id == 0, "D9 failure clears attribution");
        Check(s.default_deck_repair.attempts == 2, "D10 failure increments attempts");
        Check(wqn::TickDefaultDeckRepair(&s, CleanGates(10, 5001)) ==
                  DefaultDeckRepairTick::kDefer,
              "D11 failure backoff defers");
        Check(wqn::TickDefaultDeckRepair(&s, CleanGates(10, 9000)) ==
                  DefaultDeckRepairTick::kSubmit,
              "D12 second backoff (2x) eventually ticks kSubmit");
        // Bounded backoff: even a large attempt count caps at 30s.
        s.default_deck_repair.attempts = 40;
        wqn::OnDefaultDeckRepairSubmitted(&s, 0, 9000);
        Check(s.default_deck_repair.retry_after_ms <= 9000 + 30000,
              "D13 backoff bounded at 30s");
    }

    // -------------------------------------------------------------------
    // E. Gates preserve intent; manual/scope priority; generation cancel;
    //    ACK attribution; idempotent arm; stale-signal cancel (gap 2/3/5).
    // -------------------------------------------------------------------
    {
        WordAppState s;
        s.default_deck_id = "gone";
        s.default_deck_missing = true;
        wqn::ArmDefaultDeckRepair(&s, 10);
        // Each gate DEFERS but never drops the intent.
        wqn::DefaultDeckRepairGates g = CleanGates(10, 0);
        g.manual_switch_pending = true;
        Check(wqn::TickDefaultDeckRepair(&s, g) ==
                  DefaultDeckRepairTick::kDefer && s.default_deck_repair.pending,
              "E1 manual pending defers, intent kept");
        g = CleanGates(10, 0); g.scope_reset_pending = true;
        Check(wqn::TickDefaultDeckRepair(&s, g) ==
                  DefaultDeckRepairTick::kDefer && s.default_deck_repair.pending,
              "E2 scope-reset pending defers, intent kept");
        g = CleanGates(10, 0); g.observations_pending = true;
        Check(wqn::TickDefaultDeckRepair(&s, g) ==
                  DefaultDeckRepairTick::kDefer && s.default_deck_repair.pending,
              "E3 observations pending defers, intent kept");
        g = CleanGates(10, 0); g.persisting = true;
        Check(wqn::TickDefaultDeckRepair(&s, g) ==
                  DefaultDeckRepairTick::kDefer && s.default_deck_repair.pending,
              "E4 kPersisting defers, intent kept");
        g = CleanGates(10, 0);
        Check(wqn::TickDefaultDeckRepair(&s, g) ==
                  DefaultDeckRepairTick::kSubmit,
              "E5 all gates clear ticks kSubmit");

        // Epoch moved (committed manual switch / scope reset) -> cancel.
        Check(wqn::TickDefaultDeckRepair(&s, CleanGates(11, 0)) ==
                  DefaultDeckRepairTick::kIdle,
              "E6 epoch mismatch cancels (kIdle)");
        Check(!s.default_deck_repair.pending &&
                  !wqn::DefaultDeckRepairBlocking(s),
              "E7 epoch mismatch drops intent");

        // Deck reappeared / manual ACK resolved signal -> cancel.
        wqn::ArmDefaultDeckRepair(&s, 10);
        s.default_deck_missing = false;
        Check(wqn::TickDefaultDeckRepair(&s, CleanGates(10, 0)) ==
                  DefaultDeckRepairTick::kIdle,
              "E8 resolved signal cancels (kIdle)");
        Check(!s.default_deck_repair.pending, "E9 resolved signal drops intent");

        // Idempotent re-arm for the same epoch.
        s.default_deck_missing = true;
        Check(wqn::ArmDefaultDeckRepair(&s, 10), "E10 first arm true");
        Check(!wqn::ArmDefaultDeckRepair(&s, 10), "E11 same-epoch re-arm is no-op");
        Check(s.default_deck_repair.pending, "E12 idempotent arm keeps pending");

        // ACK attribution: a foreign (manual) op_id must NOT be stolen.
        wqn::OnDefaultDeckRepairSubmitted(&s, 99, 0);
        Check(!wqn::OnDefaultDeckRepairAck(&s, 123, ESP_OK, 0),
              "E13 foreign ACK rejected");
        Check(s.default_deck_repair.op_id == 99, "E14 foreign ACK leaves ours in flight");
        Check(wqn::OnDefaultDeckRepairAck(&s, 99, ESP_OK, 0), "E15 our ACK accepted");

        // A fresh authoritative catalog that no longer signals missing cancels
        // a stale pending intent (Arm is called on every install).
        s.default_deck_missing = false;
        Check(!wqn::ArmDefaultDeckRepair(&s, 10), "E16 non-missing catalog arm returns false");
        Check(!s.default_deck_repair.pending, "E17 non-missing catalog cancels stale intent");

        // Explicit cancel (manual Confirm path).
        s.default_deck_missing = true;
        wqn::ArmDefaultDeckRepair(&s, 10);
        wqn::CancelDefaultDeckRepair(&s);
        Check(!wqn::DefaultDeckRepairBlocking(s), "E18 explicit cancel clears");
        Check(wqn::TickDefaultDeckRepair(&s, CleanGates(10, 0)) ==
                  DefaultDeckRepairTick::kIdle,
              "E19 cancelled intent ticks idle");
    }

    // -------------------------------------------------------------------
    // F. [deck-autorepair] REVIEW-02b: an accepted op_id must survive a
    //    reappearing / bad catalog until its TERMINAL ACK. Otherwise the
    //    worker commits all-decks to NVS but the rejected ACK never installs
    //    it in memory -> NVS/memory desync.
    // -------------------------------------------------------------------
    {
        WordAppState s;
        s.default_deck_id = "gone";
        s.default_deck_missing = true;
        // pending -> submit (op attributed, not yet acked).
        Check(wqn::ArmDefaultDeckRepair(&s, /*gen=*/10), "F1 arm before submit");
        wqn::OnDefaultDeckRepairSubmitted(&s, /*op=*/501, /*now=*/0);
        Check(s.default_deck_repair.op_id == 501, "F2 op attributed in flight");
        Check(s.default_deck_repair.must_complete, "F2b accepted sets must_complete");

        // A NEW authoritative catalog shows the default deck REAPPEARED.
        // InstallWordDeckCatalog clears the missing signal, and ApplyWordCloudResult
        // calls Arm right after. Arm must NOT erase the in-flight op_id.
        std::vector<wqn::WordDeckInfo> reappeared = {{"gone", "Back", 4}};
        wqn::InstallWordDeckCatalog(&s, reappeared, /*authoritative=*/true);
        Check(!s.default_deck_missing, "F3 reappeared catalog clears signal");
        Check(!wqn::ArmDefaultDeckRepair(&s, 10), "F4 Arm on invalid signal is no-op");
        Check(s.default_deck_repair.op_id == 501,
              "F5 Arm preserves the accepted op through the reappearing catalog");

        // Cancel (manual Confirm path) must also refuse to clear a submitted op.
        wqn::CancelDefaultDeckRepair(&s);
        Check(s.default_deck_repair.op_id == 501 && s.default_deck_repair.must_complete,
              "F6 Cancel refuses to clear a submitted operation");

        // Tick must defer (in flight) rather than cancel on the invalid signal.
        Check(wqn::TickDefaultDeckRepair(&s, CleanGates(10, 0)) ==
                  DefaultDeckRepairTick::kDefer,
              "F7 in-flight tick defers even with resolved signal");

        // The success ACK is still attributed: caller installs all-decks in
        // memory so it follows the already-committed NVS state.
        Check(wqn::OnDefaultDeckRepairAck(&s, 501, ESP_OK, 0),
              "F8 success ACK is attributed to preserved op");
        Check(s.default_deck_repair.op_id == 0 && !s.default_deck_repair.pending &&
                  !s.default_deck_repair.must_complete,
              "F9 success ACK clears the whole intent + obligation");
        // Simulate the caller's durable-ACK install (SetDefaultWordDeck).
        s.default_deck_id.clear();
        s.default_deck_title.clear();
        Check(s.default_deck_id.empty(), "F10 caller installs committed all-decks");

        // A stale DUPLICATE success ACK must be rejected (op already cleared).
        Check(!wqn::OnDefaultDeckRepairAck(&s, 501, ESP_OK, 1),
              "F11 stale duplicate ACK rejected");
    }

    // -------------------------------------------------------------------
    // G. [deck-autorepair] REVIEW-02c MARKER CORRECTION: a FAILED accepted
    //    transaction may be HALF-APPLIED (default key / session clears /
    //    scope generation durable, cursor/marker tail outstanding). A
    //    reappearing/valid catalog or non-authoritative load is NOT proof of
    //    no mutation. Keep must_complete and RETRY to a success ACK.
    // -------------------------------------------------------------------
    {
        WordAppState s;
        s.default_deck_id = "gone";
        s.default_deck_missing = true;
        wqn::ArmDefaultDeckRepair(&s, 10);
        wqn::OnDefaultDeckRepairSubmitted(&s, /*op=*/602, 0);
        // New authoritative catalog: deck is back -> signal resolves.
        std::vector<wqn::WordDeckInfo> present = {{"gone", "Back", 2}};
        wqn::InstallWordDeckCatalog(&s, present, /*authoritative=*/true);
        wqn::ArmDefaultDeckRepair(&s, 10);
        Check(s.default_deck_repair.op_id == 602, "G1 op preserved after reappear");

        // FAILED ACK, with the signal now INVALID (deck reappeared).
        Check(wqn::OnDefaultDeckRepairAck(&s, 602, ESP_FAIL, 100),
              "G2 failed ACK is attributed");
        Check(s.default_deck_repair.must_complete,
              "G3 failed accepted transaction keeps recovery obligation");
        Check(s.default_deck_repair.pending && s.default_deck_repair.op_id == 0,
              "G4 failed ACK re-arms pending, clears only this op_id");
        Check(wqn::DefaultDeckRepairBlocking(s),
              "G5 gate STAYS CLOSED between failed ACK and retry");

        // A stale DUPLICATE failure ACK (already-cleared op_id) is rejected.
        Check(!wqn::OnDefaultDeckRepairAck(&s, 602, ESP_FAIL, 200),
              "G6 stale duplicate failure ACK rejected");

        // Cancel / Arm / reappearing catalog / moved epoch must NOT drop the
        // must_complete obligation.
        wqn::CancelDefaultDeckRepair(&s);
        Check(!wqn::ArmDefaultDeckRepair(&s, 10) && s.default_deck_repair.must_complete,
              "G7 Cancel/Arm cannot erase accepted_once obligation");
        Check(wqn::TickDefaultDeckRepair(&s, CleanGates(11, 100)) ==
                  DefaultDeckRepairTick::kDefer,
              "G8 epoch move cannot cancel accepted_once obligation");

        // Backoff elapses -> pump RETRIES with a fresh op_id (not idle).
        Check(wqn::TickDefaultDeckRepair(&s, CleanGates(11, 5000)) ==
                  DefaultDeckRepairTick::kSubmit,
              "G9 subsequent Tick still SUBMITS the marker-tail retry");
        wqn::OnDefaultDeckRepairSubmitted(&s, /*fresh op=*/603, 5000);
        Check(s.default_deck_repair.op_id == 603 && s.default_deck_repair.must_complete,
              "G10 retry uses a fresh operation_id, obligation retained");
        // A stale ACK for the OLD op is rejected.
        Check(!wqn::OnDefaultDeckRepairAck(&s, 602, ESP_OK, 5001),
              "G11 stale old-op ACK rejected");
        // SUCCESS on the fresh op clears the obligation + installs once.
        Check(wqn::OnDefaultDeckRepairAck(&s, 603, ESP_OK, 5001),
              "G12 success ACK of retry is attributed");
        Check(!s.default_deck_repair.must_complete && !s.default_deck_repair.pending &&
                  s.default_deck_repair.op_id == 0 && !wqn::DefaultDeckRepairBlocking(s),
              "G13 success clears obligation + opens the gates");
    }

    // -------------------------------------------------------------------
    // H. Failure ACK with the signal STILL valid keeps + backs off the intent;
    //    a transient/bad catalog load (non-authoritative) must not cancel an
    //    already accepted operation (Arm/Cancel/Tick judge obligation first).
    // -------------------------------------------------------------------
    {
        WordAppState s;
        s.default_deck_id = "gone";
        s.default_deck_missing = true;
        wqn::ArmDefaultDeckRepair(&s, 10);
        wqn::OnDefaultDeckRepairSubmitted(&s, /*op=*/703, 0);
        // A transient/bad catalog load: authoritative=false leaves the signal
        // false but must NEVER drop the accepted op (Arm checks obligation).
        wqn::InstallWordDeckCatalog(&s, {}, /*authoritative=*/false);
        wqn::ArmDefaultDeckRepair(&s, 10);
        Check(s.default_deck_repair.op_id == 703 && s.default_deck_repair.must_complete,
              "H1 failed transient catalog load keeps accepted op + obligation");
        Check(wqn::OnDefaultDeckRepairAck(&s, 703, ESP_FAIL, 1000),
              "H2 failure ACK attributed (signal invalid after bad load)");
        Check(s.default_deck_repair.must_complete && s.default_deck_repair.pending &&
                  s.default_deck_repair.attempts == 1 &&
                  s.default_deck_repair.retry_after_ms > 1000,
              "H3 invalid-signal failure STILL preserves obligation + backs off");
        Check(wqn::TickDefaultDeckRepair(&s, CleanGates(10, 1001)) ==
                  DefaultDeckRepairTick::kDefer,
              "H4 backoff defers the retry");
        Check(wqn::TickDefaultDeckRepair(&s, CleanGates(10, 9000)) ==
                  DefaultDeckRepairTick::kSubmit,
              "H5 past backoff still retries despite invalid signal");
    }

    // -------------------------------------------------------------------
    // I. Cancel/Arm cancel ONLY an UNSUBMITTED (pending) intent. Accepted-once
    //    recovery obligation survives manual priority (no cancel).
    // -------------------------------------------------------------------
    {
        WordAppState s;
        s.default_deck_id = "gone";
        s.default_deck_missing = true;
        wqn::ArmDefaultDeckRepair(&s, 10);
        Check(s.default_deck_repair.op_id == 0 && s.default_deck_repair.pending &&
                  !s.default_deck_repair.must_complete,
              "I1 unsubmitted intent is pending, no op / no obligation");
        wqn::CancelDefaultDeckRepair(&s);
        Check(!wqn::DefaultDeckRepairBlocking(s),
              "I2 Cancel clears an unsubmitted intent (manual priority)");
        // A non-signaling catalog cancels only an unsubmitted stale intent.
        wqn::ArmDefaultDeckRepair(&s, 10);
        s.default_deck_missing = false;
        Check(!wqn::ArmDefaultDeckRepair(&s, 10), "I3 Arm on resolved signal no-op");
        Check(!s.default_deck_repair.pending, "I4 unsubmitted stale intent cancelled");

        // But once accepted, manual priority (Cancel) cannot win.
        s.default_deck_missing = true;
        wqn::ArmDefaultDeckRepair(&s, 10);
        wqn::OnDefaultDeckRepairSubmitted(&s, /*op=*/804, 0);
        wqn::CancelDefaultDeckRepair(&s);
        Check(s.default_deck_repair.must_complete && wqn::DefaultDeckRepairBlocking(s),
              "I5 manual cannot cancel an accepted_once recovery obligation");
    }

    {
        wqn::WordAppState s;
        wqn::scope_ready=false;
        Check(wqn::DefaultDeckRepairBlocking(s), "unknown storage scope blocks study even without auto-repair intent");
        wqn::scope_ready=true;
        Check(!wqn::DefaultDeckRepairBlocking(s), "validated storage scope releases the independent barrier");
        s.default_deck_manual_pending=true;
        Check(wqn::DefaultDeckRepairBlocking(s), "accepted manual switch blocks through worker result installation");
        s.default_deck_manual_pending=false;
        Check(!wqn::DefaultDeckRepairBlocking(s), "installed manual result releases study admission");
    }
    std::printf("word_scope_autorepair: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
