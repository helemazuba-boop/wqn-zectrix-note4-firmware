#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include "schema_ports.h"
#include "runtime/storage_schema.h"

unsigned checks = 0, cases = 0;
#define CHECK(expr) do { ++checks; if (!(expr)) { \
    std::fprintf(stderr, "FAIL schema line %d: %s\n", __LINE__, #expr); \
    std::exit(1); } } while (false)

struct Staged {
    bool has_generation = false, has_id = false;
    uint32_t generation = 0;
    std::vector<uint8_t> id;
};
struct Environment {
    bool initialized = false, namespace_exists = true, has_generation = true;
    uint32_t generation = 3;
    std::vector<uint8_t> reset_id = std::vector<uint8_t>(16, 0xa5);
    bool mounted = false, mountable = true, observations = true, credentials = true;
    bool last_auto_format = false, allow_lease = true;
    unsigned leases = 0;
    uint32_t next_handle = 1;
    std::map<nvs_handle_t, Staged> handles;
    std::map<std::string, unsigned> calls;
    std::map<std::string, std::pair<unsigned, int>> failures;
    std::vector<std::string> trace;
} env;

void Reset() { CHECK(env.leases == 0); CHECK(env.handles.empty()); env = {}; ++cases; }
int Outcome(const char* name) {
    env.trace.emplace_back(name);
    const unsigned count = ++env.calls[name];
    const auto found = env.failures.find(name);
    return found != env.failures.end() && count == found->second.first
        ? found->second.second : ESP_OK;
}
void Fail(const char* name, unsigned occurrence = 1, int error = ESP_FAIL) {
    env.failures[name] = {occurrence, error};
}
unsigned Calls(const char* name) { return env.calls[name]; }
void HostLog(const char*, const char*, ...) {}
const char* esp_err_to_name(int) { return "host-result"; }
bool HostAcquireLease() {
    Outcome("lease");
    if (!env.allow_lease) return false;
    ++env.leases;
    return true;
}
void HostReleaseLease() { CHECK(env.leases == 1); --env.leases; }
int nvs_flash_init() {
    const int result = Outcome("init");
    env.initialized = result == ESP_OK;
    return result;
}
int nvs_flash_deinit() {
    const int result = Outcome("deinit");
    if (result != ESP_OK) return result;
    if (!env.initialized) return ESP_ERR_NVS_NOT_INITIALIZED;
    env.initialized = false;
    return ESP_OK;
}
int nvs_flash_erase() {
    const int result = Outcome("erase");
    if (result != ESP_OK) return result;
    CHECK(env.handles.empty());
    env.namespace_exists = env.has_generation = env.credentials = false;
    env.reset_id.clear();
    return ESP_OK;
}
int nvs_open(const char* name, int mode, nvs_handle_t* handle) {
    CHECK(std::string(name) == "wqn_boot");
    const int result = Outcome(mode == NVS_READONLY ? "open-ro" : "open-rw");
    if (result != ESP_OK) return result;
    if (!env.namespace_exists && mode == NVS_READONLY) return ESP_ERR_NVS_NOT_FOUND;
    if (mode == NVS_READWRITE) env.namespace_exists = true;
    *handle = env.next_handle++;
    env.handles.emplace(*handle, Staged{});
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { CHECK(env.handles.erase(handle) == 1); }
int nvs_get_u32(nvs_handle_t handle, const char* key, uint32_t* out) {
    CHECK(env.handles.count(handle)); CHECK(std::string(key) == "schema_gen");
    const int result = Outcome("get-generation");
    if (result != ESP_OK) return result;
    if (!env.has_generation) return ESP_ERR_NVS_NOT_FOUND;
    *out = env.generation;
    return ESP_OK;
}
int nvs_get_blob(nvs_handle_t handle, const char* key, void* out, size_t* bytes) {
    CHECK(env.handles.count(handle)); CHECK(std::string(key) == "reset_id");
    const int result = Outcome(out == nullptr ? "get-id-size" : "get-id-data");
    if (result != ESP_OK) return result;
    if (env.reset_id.empty()) return ESP_ERR_NVS_NOT_FOUND;
    if (out != nullptr) {
        CHECK(*bytes >= env.reset_id.size());
        std::memcpy(out, env.reset_id.data(), env.reset_id.size());
    }
    *bytes = env.reset_id.size();
    return ESP_OK;
}
int nvs_set_u32(nvs_handle_t handle, const char* key, uint32_t value) {
    CHECK(env.handles.count(handle)); CHECK(std::string(key) == "schema_gen");
    const int result = Outcome("set-generation");
    if (result != ESP_OK) return result;
    env.handles[handle].has_generation = true;
    env.handles[handle].generation = value;
    return ESP_OK;
}
int nvs_set_blob(nvs_handle_t handle, const char* key, const void* data, size_t bytes) {
    CHECK(env.handles.count(handle)); CHECK(std::string(key) == "reset_id");
    const int result = Outcome("set-id");
    if (result != ESP_OK) return result;
    const auto* first = static_cast<const uint8_t*>(data);
    env.handles[handle].id.assign(first, first + bytes);
    env.handles[handle].has_id = true;
    return ESP_OK;
}
int nvs_commit(nvs_handle_t handle) {
    CHECK(env.handles.count(handle));
    const int result = Outcome("commit");
    if (result != ESP_OK) return result;
    const Staged& staged = env.handles[handle];
    if (staged.has_generation) { env.has_generation = true; env.generation = staged.generation; }
    if (staged.has_id) env.reset_id = staged.id;
    return ESP_OK;
}
bool esp_spiffs_mounted(const char*) { return env.mounted; }
int esp_vfs_spiffs_register(const esp_vfs_spiffs_conf_t* config) {
    CHECK(std::string(config->partition_label) == "storage");
    env.last_auto_format = config->format_if_mount_failed;
    const int result = Outcome("mount");
    if (result != ESP_OK) return result;
    if (!env.mountable) return ESP_FAIL;
    env.mounted = true;
    return ESP_OK;
}
int esp_vfs_spiffs_unregister(const char*) {
    const int result = Outcome("unmount");
    if (result == ESP_OK) env.mounted = false;
    return result;
}
int esp_spiffs_info(const char*, size_t* total, size_t* used) {
    CHECK(env.mounted);
    const int result = Outcome("info");
    if (result == ESP_OK) { *total = 8 * 1024 * 1024; *used = env.observations ? 4096 : 0; }
    return result;
}
int esp_spiffs_format(const char*) {
    const int result = Outcome("format");
    if (result != ESP_OK) return result;
    env.observations = false;
    env.mountable = true;
    return ESP_OK;
}
void esp_fill_random(void* out, size_t bytes) { std::memset(out, 0x5a, bytes); }

using namespace wqn::runtime;
StorageSchemaBootResult Boot() {
    const auto result = EnsureStorageSchema();
    CHECK(env.leases == 0);
    CHECK(env.handles.empty());
    return result;
}
void ExpectReset() {
    const auto result = Boot();
    CHECK(result.action == StorageSchemaBootAction::kRestartRequired);
    CHECK(result.error == ESP_OK);
    CHECK(Calls("erase") == 1 && Calls("format") == 1);
    CHECK(!env.observations && !env.credentials);
    CHECK(env.has_generation && env.generation == kStorageSchemaGeneration);
    CHECK(env.reset_id == std::vector<uint8_t>(16, 0x5a));
    CHECK(std::string(result.reset_id) == "5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a");
    CHECK(std::strlen(result.reset_id) == 32);
    CHECK(!env.last_auto_format);
    const auto position = [](const char* name) {
        const auto found = std::find(env.trace.rbegin(), env.trace.rend(), name);
        CHECK(found != env.trace.rend());
        return env.trace.size() - 1 - std::distance(env.trace.rbegin(), found);
    };
    CHECK(position("erase") < position("format"));
    CHECK(position("format") < position("set-generation"));
    CHECK(position("set-id") < position("commit"));
}

int main() {
    Reset();
    auto result = Boot();
    CHECK(result.action == StorageSchemaBootAction::kReady && result.error == ESP_OK);
    CHECK(result.observed_generation == 3);
    CHECK(env.observations && env.credentials);
    CHECK(Calls("erase") == 0 && Calls("format") == 0 && Calls("commit") == 0);
    CHECK(!env.last_auto_format);

    Reset(); env.namespace_exists = env.has_generation = env.observations = false;
    env.reset_id.clear(); env.mountable = false;
    ExpectReset();
    result = Boot();
    CHECK(result.action == StorageSchemaBootAction::kReady);
    CHECK(Calls("erase") == 1 && Calls("format") == 1);

    // Characterization: all these current decisions destroy local records.
    // Policy patches must explicitly revise this table and its expected data fate.
    for (int scenario = 0; scenario < 10; ++scenario) {
        Reset();
        switch (scenario) {
            case 0: env.namespace_exists = env.has_generation = false; env.reset_id.clear(); break;
            case 1: env.generation = 2; break;
            case 2: env.generation = 4; break;
            case 3: env.generation = 0; break;
            case 4: env.reset_id.resize(15); break;
            case 5: env.reset_id.clear(); break;
            case 6: env.has_generation = false; break;
            case 7: Fail("init", 1, ESP_ERR_NVS_NO_FREE_PAGES); break;
            case 8: Fail("init", 1, ESP_ERR_NVS_NEW_VERSION_FOUND); break;
            case 9: env.mounted = true; env.generation = 2; break;
        }
        ExpectReset();
    }
    for (const char* fault : {"open-ro", "get-generation", "get-id-size", "get-id-data"}) {
        Reset(); Fail(fault, 1, ESP_ERR_INVALID_CRC); ExpectReset();
    }
    for (const char* fault : {"mount", "info", "unmount"}) {
        Reset(); Fail(fault);
        result = Boot();
        CHECK(result.action == StorageSchemaBootAction::kRecoveryRequired);
        CHECK(result.error == ESP_FAIL && result.observed_generation == 3);
        CHECK(Calls("erase") == 0 && Calls("format") == 0);
        CHECK(env.observations && env.credentials);
        CHECK(!env.last_auto_format);
    }
    for (const char* fault : {"deinit", "erase", "init", "format", "mount", "info",
                              "unmount", "open-rw", "set-generation", "set-id", "commit"}) {
        Reset(); env.generation = 2;
        Fail(fault, std::string(fault) == "init" ? 2 : 1);
        result = Boot();
        CHECK(result.action == StorageSchemaBootAction::kRecoveryRequired);
        CHECK(result.error == ESP_FAIL);
    }
    for (const char* fault : {"format", "mount", "set-generation", "set-id", "commit"}) {
        Reset(); env.generation = 2; Fail(fault);
        result = Boot();
        CHECK(result.action == StorageSchemaBootAction::kRecoveryRequired);
        env.failures.clear();
        result = Boot();
        CHECK(result.action == StorageSchemaBootAction::kRestartRequired);
        result = Boot();
        CHECK(result.action == StorageSchemaBootAction::kReady);
    }
    Reset(); env.allow_lease = false;
    result = Boot();
    CHECK(result.action == StorageSchemaBootAction::kRecoveryRequired);
    CHECK(result.error == ESP_ERR_INVALID_STATE && Calls("init") == 0);
    Reset(); Fail("init");
    result = Boot();
    CHECK(result.action == StorageSchemaBootAction::kRecoveryRequired);
    CHECK(Calls("erase") == 0 && env.observations);

    Reset();
    CHECK(InvalidateStorageSchemaForFactoryReset() == ESP_OK);
    CHECK(env.generation == 0 && env.observations && env.credentials);
    ExpectReset();
    Reset(); Fail("commit");
    CHECK(InvalidateStorageSchemaForFactoryReset() == ESP_FAIL);
    CHECK(env.generation == 3 && env.observations && env.credentials);
    CHECK(env.handles.empty());
    std::printf("PASS: boot schema %u checks in %u host scenarios; current policy characterized, not HIL\n",
                checks, cases);
}
