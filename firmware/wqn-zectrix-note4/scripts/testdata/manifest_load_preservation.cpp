#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include "cJSON.h"
#include "device_protocol/json_depth_guard.h"
#include "note_cloud.h"
#include "problem_cloud.h"
#include "word_pack.h"

#define ESP_LOGW(...) ((void)0)

namespace {
constexpr uint64_t kPackSchemaVersion = wqn::protocol::note_study_v1::kPackSchemaVersion;
constexpr size_t kMaxStoredNotebooks = 200;
constexpr size_t kMaxPackEntries = wqn::protocol::note_study_v1::kMaxPackEntries;
constexpr size_t kMaxPackBytes = wqn::protocol::note_study_v1::kMaxPackBytes;
@@DECODER@@

esp_err_t load_error = ESP_OK, prerequisite = ESP_OK;
size_t resets = 0, fetches = 0, loads = 0, checks = 0, failures = 0;
std::string retained_bytes, last_message;
bool use_real_decoder = false, fail_allocation = false;
constexpr char kRetained[] = "primary|backup|old-pack|outbox|session";
struct ProbeResult { esp_err_t result = ESP_OK; std::string message; };

void Check(bool ok, const char* message)
{
    ++checks;
    if (!ok) {
        if (failures < 10) std::fprintf(stderr, "FAIL %s\n", message);
        ++failures;
    }
}
void Reset()
{
    resets = fetches = loads = 0;
    load_error = prerequisite = ESP_OK;
    retained_bytes = kRetained;
    last_message.clear();
    use_real_decoder = fail_allocation = false;
}
void* Allocate(size_t bytes) { return fail_allocation ? nullptr : std::malloc(bytes); }
} // namespace

namespace wqn {
esp_err_t LoadNotePackManifest(WqnNotePackManifest* manifest)
{
    ++loads;
    return use_real_decoder ? ParseNoteManifestPayload(
        "{\"data\":{\"cursor\":0,\"has_more\":false,\"notebooks\":[]}}", manifest) : load_error;
}
esp_err_t LoadProblemPackManifest(WqnProblemPackManifest*) { ++loads; return load_error; }
esp_err_t LoadWordPackManifest(WqnWordPackManifest*) { ++loads; return load_error; }
esp_err_t ResetNotePackStorageCache() { ++resets; retained_bytes.clear(); return ESP_OK; }
esp_err_t ResetProblemPackStorageCache() { ++resets; retained_bytes.clear(); return ESP_OK; }
esp_err_t ResetWordPackStorageCache() { ++resets; retained_bytes.clear(); return ESP_OK; }
@@ADMISSION@@
} // namespace wqn

int main()
{
    cJSON_Hooks hooks{Allocate, std::free};
    cJSON_InitHooks(&hooks);
    for (auto probe : {wqn::ProbeNote, wqn::ProbeProblem, wqn::ProbeWord}) {
        for (esp_err_t error : {ESP_ERR_NO_MEM, ESP_FAIL, ESP_ERR_INVALID_RESPONSE,
                               ESP_ERR_INVALID_SIZE, ESP_ERR_INVALID_STATE}) {
            Reset();
            load_error = error;
            Check(probe() == error, "unclassified load failure is propagated");
            Check(resets == 0 && retained_bytes == kRetained, "load failure preserves all cache bytes");
            Check(fetches == 0, "load failure stops before remote sync");
            Check(!last_message.empty(), "load failure has visible status");
        }
        for (esp_err_t error : {ESP_OK, ESP_ERR_NOT_FOUND}) {
            Reset();
            load_error = error;
            Check(probe() == ESP_OK, "valid or confirmed-missing manifest permits sync");
            Check(resets == 0 && retained_bytes == kRetained, "admission never erases cache");
            Check(fetches == 1, "first-install and healthy paths proceed");
        }
    }
    Reset();
    use_real_decoder = true;
    fail_allocation = true;
    Check(wqn::ProbeNote() == ESP_ERR_INVALID_RESPONSE, "real Note cJSON OOM remains ambiguous with malformed JSON");
    Check(resets == 0 && retained_bytes == kRetained && fetches == 0, "ambiguous real parser OOM cannot authorize cache erasure");
    fail_allocation = false;
    Check(wqn::ProbeNote() == ESP_OK && fetches == 1, "same valid manifest succeeds after allocator recovers");
    for (esp_err_t error : {ESP_FAIL, ESP_ERR_NOT_FOUND, ESP_ERR_INVALID_RESPONSE}) {
        Reset();
        prerequisite = error;
        Check(wqn::ProbeWord() == error, "failed install marker is preserved");
        Check(loads == 0 && resets == 0 && fetches == 0, "failed install marker admits no domain I/O");
    }
    std::printf("%zu checks, %zu failures (real admission blocks and Note decoder; host ports; NOT HIL)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
