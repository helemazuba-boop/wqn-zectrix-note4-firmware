// Function-level host fixture for the default-deck scope-change recoverable
// tail. NVS is mocked (an in-memory map + fault injection); libc/flash,
// StorageService concurrency and physical power loss are NOT exercised.
// The two production functions under test are extracted verbatim from
// main/storage.cpp by test_word_deck_scope_tail.py.
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_INVALID_ARG = 0x102;
constexpr esp_err_t ESP_ERR_NVS_NOT_FOUND = 0x1102;
constexpr const char* kTag = "scope-fixture";
const char* esp_err_to_name(esp_err_t) { return "injected-error"; }
void FixtureLog(const char*, const char*, ...) {}
#define ESP_LOGW(...) FixtureLog(__VA_ARGS__)
#define ESP_LOGI(...) FixtureLog(__VA_ARGS__)
#define ESP_RETURN_ON_ERROR(expr, tag, msg) do { \
    esp_err_t _e = (expr); if (_e != ESP_OK) return _e; \
} while (0)

// The production functions live in namespace wqn and reference
// protocol::word_study_v1::Mode (see main/storage.cpp:950).
namespace wqn {
namespace protocol::word_study_v1 {
enum class Mode : uint8_t {
    kSequential, kRandom, kDictionary, kReview, kIntake, kShuffle, kMistakes,
};
}
}
using Mode = wqn::protocol::word_study_v1::Mode;
const char* ModeKey(Mode m) {
    switch (m) {
        case Mode::kSequential: return "sequential";
        case Mode::kReview: return "review";
        case Mode::kShuffle: return "shuffle";
        case Mode::kMistakes: return "mistakes";
        default: return "other";
    }
}

// ---- Mocked NVS namespace -------------------------------------------------
// u64 values are stored as decimal strings, matching nvs_set_u64/get_u64
// semantics closely enough for this ordering/idempotency test.
struct FakeNvs {
    std::map<std::string, std::string> kv;
    std::vector<std::string> log;
    // Fault injection: (op, key) pair that fails. Empty = no fault.
    std::string fail_op;
    std::string fail_key;

    void Reset(const std::string& gen, const std::string& cursor,
               const std::string& marker, const std::string& deck) {
        kv.clear(); log.clear(); fail_op.clear(); fail_key.clear();
        kv["word_deck_gen"] = gen;
        kv["word_seq_idx"] = cursor;
        kv["word_deck_chg"] = marker;
        kv["word_deck"] = deck;
    }
    bool ShouldFail(const char* op, const char* key) {
        log.push_back(std::string(op) + ":" + key);
        if (fail_op == op && fail_key == key) return true;
        return false;
    }
    int IndexOf(const std::string& entry) const {
        for (size_t i = 0; i < log.size(); ++i)
            if (log[i] == entry) return static_cast<int>(i);
        return -1;
    }
} fake;

esp_err_t ClearPersistedWordSession(Mode mode) {
    char key[64];
    std::snprintf(key, sizeof(key), "session:%s", ModeKey(mode));
    if (fake.ShouldFail("session", key)) return ESP_FAIL;
    return ESP_OK;
}
esp_err_t SaveStringToNvsRaw(const char* key, const std::string& value) {
    if (fake.ShouldFail("set_str", key)) return ESP_FAIL;
    fake.kv[key] = value;
    return ESP_OK;
}
esp_err_t SaveU64ToNvsRaw(const char* key, uint64_t value) {
    if (fake.ShouldFail("set_u64", key)) return ESP_FAIL;
    fake.kv[key] = std::to_string(value);
    return ESP_OK;
}
esp_err_t ClearNvsKeyRaw(const char* key) {
    if (fake.ShouldFail("clear", key)) return ESP_FAIL;
    fake.kv.erase(key);  // missing key is ESP_OK, matching production
    return ESP_OK;
}
esp_err_t LoadU64FromNvs(const char* key, uint64_t* value, bool* found) {
    *value = 0; *found = false;
    auto it = fake.kv.find(key);
    if (it == fake.kv.end()) return ESP_OK;
    *value = std::strtoull(it->second.c_str(), nullptr, 10);
    *found = true;
    return ESP_OK;
}
esp_err_t LoadStringFromNvs(const char* key, std::string* value) {
    value->clear();
    auto it = fake.kv.find(key);
    if (it == fake.kv.end()) return ESP_OK;
    *value = it->second;
    return ESP_OK;
}

std::atomic<uint32_t> g_deck_scope_generation{0};
constexpr const char* kDeckScopeMarkerKey = "word_deck_chg";
constexpr const char* kDeckScopeGenKey = "word_deck_gen";
constexpr const char* kDefaultWordDeckKey = "word_deck";
constexpr const char* kWordSequentialCursorKey = "word_seq_idx";

// ---- Production functions under test (verbatim, inside namespace wqn) -----
namespace wqn {

@@APPLY@@

@@RECOVER@@

}  // namespace wqn
using wqn::ApplyDeckScopeChangeLocked;
using wqn::RecoverDeckScopeTransaction;

// ---- Test harness ---------------------------------------------------------
int passes = 0, failures = 0;
void Check(bool condition, const char* name) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
    condition ? ++passes : ++failures;
}

const char* kDeckId =
    "11111111-2222-3333-4444-555555555555";  // 36 chars

int main() {
    // A. Happy path: cursor reset precedes marker clear, everything commits.
    fake.Reset("5", "123", "6|" + std::string(kDeckId), "old-deck");
    g_deck_scope_generation.store(5, std::memory_order_release);
    esp_err_t r = ApplyDeckScopeChangeLocked(6, kDeckId);
    Check(r == ESP_OK, "A: default-deck tail returns ESP_OK");
    Check(fake.IndexOf("set_u64:word_seq_idx") >= 0,
          "A: sequential cursor u64 write attempted");
    Check(fake.IndexOf("clear:word_deck_chg") >= 0,
          "A: marker clear attempted");
    Check(fake.IndexOf("set_u64:word_seq_idx") <
              fake.IndexOf("clear:word_deck_chg"),
          "A: cursor reset happens BEFORE marker clear");
    Check(fake.IndexOf("set_u64:word_seq_idx") >
              fake.IndexOf("set_u64:word_deck_gen"),
          "A: cursor reset happens AFTER scope generation commit");
    Check(fake.kv["word_seq_idx"] == "0", "A: cursor NVS value is 0");
    Check(fake.kv.count("word_deck_chg") == 0, "A: marker erased");
    Check(fake.kv["word_deck_gen"] == "6", "A: new generation saved");
    Check(fake.kv["word_deck"] == kDeckId, "A: new deck id saved");
    Check(g_deck_scope_generation.load(std::memory_order_acquire) == 6,
          "A: RAM generation cache updated");
    int sessions = 0;
    for (const auto& e : fake.log)
        if (e.rfind("session:", 0) == 0) ++sessions;
    Check(sessions == 4, "A: all four session modes cleared");

    // B. Cursor write fails: marker must remain so boot recovery can replay.
    fake.Reset("5", "123", "6|" + std::string(kDeckId), "old-deck");
    g_deck_scope_generation.store(5, std::memory_order_release);
    fake.fail_op = "set_u64"; fake.fail_key = "word_seq_idx";
    r = ApplyDeckScopeChangeLocked(6, kDeckId);
    Check(r != ESP_OK, "B: cursor-write failure propagates");
    Check(fake.kv.count("word_deck_chg") == 1,
          "B: marker preserved after cursor-write failure (recoverable)");
    Check(fake.kv["word_seq_idx"] == "123",
          "B: cursor NOT left cleared on write failure");

    // C. Marker clear fails, then RecoverDeckScopeTransaction replays the tail
    //    and finishes (covering both the cursor and marker failure windows).
    fake.Reset("5", "123", "6|" + std::string(kDeckId), "old-deck");
    g_deck_scope_generation.store(5, std::memory_order_release);
    fake.fail_op = "clear"; fake.fail_key = "word_deck_chg";
    r = ApplyDeckScopeChangeLocked(6, kDeckId);
    Check(r != ESP_OK, "C: marker-clear failure propagates");
    Check(fake.kv["word_seq_idx"] == "0",
          "C: cursor already reset before marker-clear failure");
    Check(fake.kv.count("word_deck_chg") == 1,
          "C: marker still present for recovery");
    fake.fail_op.clear(); fake.fail_key.clear();
    r = RecoverDeckScopeTransaction(nullptr);
    Check(r == ESP_OK, "C: recovery replay returns ESP_OK");
    Check(fake.kv.count("word_deck_chg") == 0,
          "C: recovery clears the marker");
    Check(fake.kv["word_seq_idx"] == "0",
          "C: recovery re-runs the cursor reset idempotently");
    Check(g_deck_scope_generation.load(std::memory_order_acquire) == 6,
          "C: recovery installs the target generation");

    // D. Idempotency: running the whole tail twice yields the same final state.
    fake.Reset("5", "999", "6|" + std::string(kDeckId), "old-deck");
    g_deck_scope_generation.store(5, std::memory_order_release);
    Check(ApplyDeckScopeChangeLocked(6, kDeckId) == ESP_OK, "D: first run OK");
    const std::string after_first = fake.kv["word_seq_idx"] + "|" +
        fake.kv["word_deck_gen"] + "|" + fake.kv["word_deck"];
    Check(ApplyDeckScopeChangeLocked(6, kDeckId) == ESP_OK, "D: second run OK");
    const std::string after_second = fake.kv["word_seq_idx"] + "|" +
        fake.kv["word_deck_gen"] + "|" + fake.kv["word_deck"];
    Check(after_first == after_second && fake.kv["word_seq_idx"] == "0",
          "D: double application is idempotent");

    // E. Recovery with no marker is a no-op that seeds generation from NVS.
    fake.Reset("9", "77", "", "");  // empty marker
    g_deck_scope_generation.store(0, std::memory_order_release);
    r = RecoverDeckScopeTransaction(nullptr);
    Check(r == ESP_OK, "E: no-marker recovery returns ESP_OK");
    Check(g_deck_scope_generation.load(std::memory_order_acquire) == 9,
          "E: committed generation seeded into RAM cache");
    Check(fake.kv["word_seq_idx"] == "77",
          "E: no-marker recovery leaves cursor untouched");

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
