#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "schema_ports.h"
#include "runtime/storage_schema.h"

unsigned checks = 0, cases = 0;
#define CHECK(expr) do { ++checks; if (!(expr)) { \
    std::fprintf(stderr, "FAIL schema line %d: %s\n", __LINE__, #expr); \
    std::exit(1); } } while (false)
using Bytes = std::vector<uint8_t>;
struct Value {
    nvs_type_t type;
    Bytes bytes;
    bool operator==(const Value& other) const { return type == other.type && bytes == other.bytes; }
};
using Keys = std::map<std::string, Value>;
using Nvs = std::map<std::string, Keys>;
struct Handle {
    std::string space;
    int mode;
    Keys staged;
    std::set<std::string> removed;
    bool clear = false;
};
struct Fault { unsigned occurrence; int error; bool after; };
struct HostIterator { std::vector<nvs_entry_info_t> entries; size_t index = 0; };
struct Environment {
    Nvs data;
    std::map<std::string, Bytes> files;
    bool immediate = false, initialized = false, mounted = false, mountable = true;
    bool last_auto_format = false, allow_lease = true, has_partition = true;
    esp_partition_t partition{3073}; // Includes a final partial read chunk.
    Bytes flash = Bytes(partition.size, 0xff);
    unsigned leases = 0, random_calls = 0;
    uint32_t next_handle = 1;
    std::map<nvs_handle_t, Handle> handles;
    std::set<nvs_iterator_t> iterators;
    std::map<std::string, unsigned> calls;
    std::map<std::string, Fault> failures;
    std::vector<std::string> trace;
    std::string short_blob;
} env;
Value U32(uint32_t number) {
    Bytes bytes(sizeof(number)); std::memcpy(bytes.data(), &number, bytes.size());
    return {NVS_TYPE_U32, bytes};
}
Bytes Intent(uint8_t id = 0x5a) {
    Bytes bytes{'W', 'Q', 'N', 'R', 'S', 'T', 1, 0, 3, 0, 0, 0};
    bytes.insert(bytes.end(), 16, id); return bytes;
}
void SetGeneration(uint32_t number) { env.data["wqn_boot"]["schema_gen"] = U32(number); }
void SetIntent(uint8_t id = 0x5a) {
    env.data["wqn_boot"]["reset_pending"] = {NVS_TYPE_BLOB, Intent(id)};
}
bool HasIntent() { return env.data["wqn_boot"].count("reset_pending") != 0; }
void Reset(bool immediate) {
    CHECK(env.leases == 0 && env.handles.empty() && env.iterators.empty());
    env = {}; env.immediate = immediate; ++cases;
    SetGeneration(3);
    env.data["wqn_boot"]["reset_id"] = {NVS_TYPE_BLOB, Bytes(16, 0xa5)};
    env.data["net"]["token"] = {NVS_TYPE_BLOB, {1, 2, 3}};
    env.data["net"]["ssid"] = {NVS_TYPE_BLOB, {4, 5, 6}};
    env.data["audio"]["volume"] = U32(75);
    env.data["zz_wifi"]["config"] = {NVS_TYPE_BLOB, {7, 8}};
    env.files = {{"word.outbox", {1, 2}}, {"note.outbox", {3, 4}}, {"problem.outbox", {5, 6}}};
    env.flash.back() = 0x42;
}
void Fail(const std::string& name, unsigned occurrence = 1, int error = ESP_FAIL, bool after = false) {
    env.failures[name] = {occurrence, error, after};
}
unsigned Calls(const std::string& name) { return env.calls[name]; }
int Outcome(const std::string& name, bool after = false) {
    if (!after) { env.trace.push_back(name); ++env.calls[name]; }
    auto it = env.failures.find(name);
    return it != env.failures.end() && it->second.occurrence == Calls(name) &&
        it->second.after == after ? it->second.error : ESP_OK;
}
void HostLog(const char*, const char*, ...) {}
const char* esp_err_to_name(int) { return "host-result"; }
bool HostAcquireLease() {
    Outcome("lease"); if (!env.allow_lease) return false;
    ++env.leases; return true;
}
void HostReleaseLease() { CHECK(env.leases == 1); --env.leases; }
int nvs_flash_init() {
    const int result = Outcome("init"); env.initialized = result == ESP_OK; return result;
}
// Old ports let --source demonstrate pre-patch destructive behavior too.
int nvs_flash_deinit() { env.initialized = false; return ESP_OK; }
int nvs_flash_erase() { Outcome("whole-erase"); env.data.clear(); return ESP_OK; }
int nvs_open(const char* space, int mode, nvs_handle_t* handle) {
    const auto op = std::string(mode == NVS_READONLY ? "open-ro:" : "open-rw:") + space;
    const int result = Outcome(op); if (result != ESP_OK) return result;
    if (!env.data.count(space) && mode == NVS_READONLY) return ESP_ERR_NVS_NOT_FOUND;
    if (mode == NVS_READWRITE) env.data[space];
    *handle = env.next_handle++;
    env.handles.emplace(*handle, Handle{space, mode, {}, {}, false}); return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { CHECK(env.handles.erase(handle) == 1); }
int nvs_get_u32(nvs_handle_t handle, const char* key, uint32_t* out) {
    CHECK(env.handles.count(handle));
    const int result = Outcome(std::string("get:") + key); if (result != ESP_OK) return result;
    auto& keys = env.data[env.handles.at(handle).space];
    auto it = keys.find(key); if (it == keys.end()) return ESP_ERR_NVS_NOT_FOUND;
    CHECK(it->second.type == NVS_TYPE_U32 && it->second.bytes.size() == sizeof(*out));
    std::memcpy(out, it->second.bytes.data(), sizeof(*out)); return ESP_OK;
}
int nvs_get_blob(nvs_handle_t handle, const char* key, void* out, size_t* size) {
    CHECK(env.handles.count(handle));
    const int result = Outcome(std::string(out ? "get-data:" : "get-size:") + key);
    if (result != ESP_OK) return result;
    const auto& keys = env.data[env.handles.at(handle).space];
    const auto it = keys.find(key); if (it == keys.end()) return ESP_ERR_NVS_NOT_FOUND;
    CHECK(it->second.type == NVS_TYPE_BLOB);
    if (out) {
        CHECK(*size >= it->second.bytes.size());
        const size_t count = it->second.bytes.size() - (env.short_blob == key ? 1 : 0);
        std::memcpy(out, it->second.bytes.data(), count);
        *size = count;
        return ESP_OK;
    }
    *size = it->second.bytes.size(); return ESP_OK;
}
int Write(nvs_handle_t handle, const char* key, Value value) {
    CHECK(env.handles.count(handle));
    auto& h = env.handles.at(handle); CHECK(h.mode == NVS_READWRITE);
    const auto op = std::string("set:") + key;
    int result = Outcome(op); if (result != ESP_OK) return result;
    if (env.immediate) env.data[h.space][key] = std::move(value);
    else h.staged[key] = std::move(value);
    return Outcome(op, true);
}
int nvs_set_u32(nvs_handle_t handle, const char* key, uint32_t value) {
    return Write(handle, key, U32(value));
}
int nvs_set_blob(nvs_handle_t handle, const char* key, const void* bytes, size_t size) {
    auto* start = static_cast<const uint8_t*>(bytes);
    return Write(handle, key, {NVS_TYPE_BLOB, Bytes(start, start + size)});
}
int nvs_erase_key(nvs_handle_t handle, const char* key) {
    auto& h = env.handles.at(handle); CHECK(h.mode == NVS_READWRITE && env.iterators.empty());
    const auto op = std::string("erase:") + key;
    const int result = Outcome(op); if (result != ESP_OK) return result;
    if (!env.data[h.space].count(key)) return ESP_ERR_NVS_NOT_FOUND;
    if (env.immediate) env.data[h.space].erase(key); else h.removed.insert(key);
    return Outcome(op, true);
}
int nvs_erase_all(nvs_handle_t handle) {
    auto& h = env.handles.at(handle); CHECK(h.mode == NVS_READWRITE);
    CHECK(h.space != "wqn_boot" && env.iterators.empty()); CHECK(HasIntent());
    const auto op = "clear:" + h.space;
    const int result = Outcome(op); if (result != ESP_OK) return result;
    if (env.immediate) env.data[h.space].clear(); else h.clear = true;
    return Outcome(op, true);
}
int nvs_commit(nvs_handle_t handle) {
    auto& h = env.handles.at(handle); const auto op = "commit:" + h.space;
    const int result = Outcome(op); if (result != ESP_OK) return result;
    if (h.clear) env.data[h.space].clear();
    for (const auto& key : h.removed) env.data[h.space].erase(key);
    for (const auto& entry : h.staged) env.data[h.space][entry.first] = entry.second;
    h.staged.clear(); h.removed.clear(); h.clear = false; return Outcome(op, true);
}
int nvs_entry_find(const char* part, const char* space, nvs_type_t type, nvs_iterator_t* out) {
    CHECK(std::string(part) == "nvs" && space == nullptr && type == NVS_TYPE_ANY);
    CHECK(env.iterators.empty()); *out = nullptr;
    const int result = Outcome("find"); if (result != ESP_OK) return result;
    std::vector<nvs_entry_info_t> entries;
    for (const auto& ns : env.data) for (const auto& kv : ns.second) {
        if (kv.second.type == NVS_TYPE_LEGACY) continue;
        nvs_entry_info_t entry;
        std::snprintf(entry.namespace_name, sizeof(entry.namespace_name), "%s", ns.first.c_str());
        std::snprintf(entry.key, sizeof(entry.key), "%s", kv.first.c_str());
        entry.type = kv.second.type; entries.push_back(entry);
    }
    if (entries.empty()) return ESP_ERR_NVS_NOT_FOUND;
    *out = new HostIterator{std::move(entries), 0}; env.iterators.insert(*out); return ESP_OK;
}
size_t KeyEntries(const Keys& keys) {
    size_t count = 0;
    for (const auto& item : keys) {
        count += item.second.type == NVS_TYPE_U32 ? 1 : 2 + (item.second.bytes.size() + 31) / 32;
    }
    return count;
}
int nvs_get_stats(const char* part, nvs_stats_t* stats) {
    CHECK(std::string(part) == "nvs");
    const int result = Outcome("stats"); if (result != ESP_OK) return result;
    stats->namespace_count = env.data.size(); stats->used_entries = env.data.size();
    for (const auto& ns : env.data) stats->used_entries += KeyEntries(ns.second);
    return ESP_OK;
}
int nvs_get_used_entry_count(nvs_handle_t handle, size_t* count) {
    const int result = Outcome("used-count"); if (result != ESP_OK) return result;
    *count = KeyEntries(env.data.at(env.handles.at(handle).space)); return ESP_OK;
}
void nvs_release_iterator(nvs_iterator_t it) {
    if (it) { CHECK(env.iterators.erase(it) == 1); delete it; }
}
int nvs_entry_next(nvs_iterator_t* it) {
    CHECK(env.iterators.count(*it)); const int result = Outcome("next");
    if (result != ESP_OK || ++(*it)->index == (*it)->entries.size()) {
        nvs_release_iterator(*it); *it = nullptr;
        return result == ESP_OK ? ESP_ERR_NVS_NOT_FOUND : result;
    }
    return ESP_OK;
}
int nvs_entry_info(nvs_iterator_t it, nvs_entry_info_t* out) {
    CHECK(env.iterators.count(it)); const int result = Outcome("entry-info");
    if (result == ESP_OK) *out = it->entries.at(it->index);
    return result;
}
const esp_partition_t* esp_partition_find_first(int type, int subtype, const char* name) {
    CHECK(type == ESP_PARTITION_TYPE_DATA && subtype == ESP_PARTITION_SUBTYPE_DATA_SPIFFS);
    CHECK(std::string(name) == "storage"); Outcome("partition");
    return env.has_partition ? &env.partition : nullptr;
}
int esp_partition_read(const esp_partition_t* part, size_t offset, void* out, size_t size) {
    CHECK(part == &env.partition && offset + size <= env.flash.size());
    const int result = Outcome("partition-read");
    if (result == ESP_OK) std::memcpy(out, env.flash.data() + offset, size);
    return result;
}
bool esp_spiffs_mounted(const char*) { return env.mounted; }
int esp_vfs_spiffs_register(const esp_vfs_spiffs_conf_t* config) {
    CHECK(std::string(config->partition_label) == "storage");
    env.last_auto_format = config->format_if_mount_failed;
    const int result = Outcome("mount"); if (result != ESP_OK) return result;
    if (!env.mountable) return ESP_FAIL;
    env.mounted = true; return ESP_OK;
}
int esp_vfs_spiffs_unregister(const char*) {
    const int result = Outcome("unmount"); if (result == ESP_OK) env.mounted = false;
    return result;
}
int esp_spiffs_info(const char*, size_t* total, size_t* used) {
    CHECK(env.mounted); const int result = Outcome("info");
    if (result == ESP_OK) { *total = env.partition.size; *used = env.files.size() * 128; }
    return result;
}
int esp_spiffs_format(const char*) {
    const int result = Outcome("format"); if (result != ESP_OK) return result;
    env.files.clear(); env.mountable = true; env.flash[0] = 0x55;
    return Outcome("format", true);
}
void esp_fill_random(void* out, size_t size) { ++env.random_calls; std::memset(out, 0x5a, size); }
using namespace wqn::runtime;
StorageSchemaBootResult Boot() {
    const auto result = EnsureStorageSchema();
    CHECK(env.leases == 0 && env.handles.empty() && env.iterators.empty());
    CHECK(Calls("whole-erase") == 0 && !env.last_auto_format); return result;
}
void ExpectPreserved(int error = ESP_ERR_INVALID_STATE) {
    const auto data = env.data; const auto files = env.files; const auto flash = env.flash;
    const auto result = Boot();
    CHECK(result.action == StorageSchemaBootAction::kRecoveryRequired && result.error == error);
    CHECK(env.data == data && env.files == files && env.flash == flash);
    CHECK(Calls("format") == 0 && Calls("commit:wqn_boot") == 0);
}
void Reboot() {
    CHECK(env.handles.empty() && env.iterators.empty());
    env.initialized = env.mounted = false; env.failures.clear(); env.calls.clear(); env.trace.clear();
}
void ExpectComplete(uint8_t id = 0x5a) {
    auto result = Boot();
    CHECK(result.action == StorageSchemaBootAction::kRestartRequired && result.error == ESP_OK);
    CHECK(Calls("format") == 1 && env.files.empty() && !HasIntent());
    for (const auto& space : env.data) CHECK(space.first == "wqn_boot" || space.second.empty());
    CHECK(env.data["wqn_boot"]["schema_gen"] == U32(kStorageSchemaGeneration));
    CHECK(env.data["wqn_boot"]["reset_id"].bytes == Bytes(16, id));
    CHECK(std::strlen(result.reset_id) == 32);
    const auto position = [](const std::string& name) {
        auto it = std::find(env.trace.begin(), env.trace.end(), name);
        CHECK(it != env.trace.end()); return std::distance(env.trace.begin(), it);
    };
    CHECK(position("format") < position("set:schema_gen"));
    CHECK(position("set:reset_id") < position("erase:reset_pending"));
    Reboot(); result = Boot();
    CHECK(result.action == StorageSchemaBootAction::kReady && result.error == ESP_OK);
    CHECK(Calls("format") == 0 && Calls("commit:wqn_boot") == 0 && Calls("partition-read") == 0);
}
void EmptyMedia() {
    env.data.clear(); env.files.clear(); env.flash.assign(env.partition.size, 0xff); env.mountable = false;
}
void Run(bool immediate) {
    Reset(immediate);
    const auto original_data = env.data;
    const auto original_files = env.files;
    auto result = Boot();
    CHECK(result.action == StorageSchemaBootAction::kReady && result.observed_generation == 3);
    CHECK(env.files.size() == 3 && Calls("format") == 0 && Calls("commit:wqn_boot") == 0);
    CHECK(Calls("partition-read") == 0);
    CHECK(env.data == original_data && env.files == original_files);
    for (uint32_t generation : {0u, 1u, 2u, 4u, 0xffffffffu}) {
        Reset(immediate); SetGeneration(generation);
        if (generation == 0) env.data["wqn_boot"].erase("reset_id");
        ExpectPreserved();
    }
    for (int shape = 0; shape < 7; ++shape) {
        Reset(immediate);
        if (shape == 0) env.data.erase("wqn_boot");
        if (shape == 1) env.data["wqn_boot"].erase("schema_gen");
        if (shape == 2) env.data["wqn_boot"].erase("reset_id");
        if (shape >= 3) env.data["wqn_boot"]["reset_id"].bytes.resize(shape == 3 ? 15 : 17);
        if (shape == 5) env.data["wqn_boot"].erase("schema_gen");
        if (shape == 6) SetGeneration(0);
        ExpectPreserved();
    }
    for (int error : {ESP_FAIL, ESP_ERR_NVS_NO_FREE_PAGES, ESP_ERR_NVS_NEW_VERSION_FOUND}) {
        Reset(immediate); Fail("init", 1, error); ExpectPreserved(error);
        CHECK(Calls("open-ro:wqn_boot") == 0);
    }
    for (const char* fault : {"open-ro:wqn_boot", "get:schema_gen", "get-size:reset_id",
                             "get-data:reset_id", "get-size:reset_pending", "get-data:reset_pending"}) {
        Reset(immediate); SetIntent(); Fail(fault, 1, ESP_ERR_INVALID_CRC);
        ExpectPreserved(ESP_ERR_INVALID_CRC);
    }
    for (int shape = 0; shape < 5; ++shape) {
        Reset(immediate); SetIntent(); auto& blob = env.data["wqn_boot"]["reset_pending"].bytes;
        if (shape == 0) blob.resize(27);
        if (shape == 1) blob.push_back(0);
        if (shape == 2) blob[0] ^= 1;
        if (shape == 3) blob[6] = 2;
        if (shape == 4) blob[8] = 4;
        ExpectPreserved();
    }
    Reset(immediate); SetIntent(); SetGeneration(4); ExpectPreserved();
    Reset(immediate); env.short_blob = "reset_id"; ExpectPreserved();
    Reset(immediate); SetIntent(); env.short_blob = "reset_pending"; ExpectPreserved();
    for (const char* fault : {"mount", "info", "unmount"}) {
        Reset(immediate); Fail(fault); ExpectPreserved(ESP_FAIL);
    }
    Reset(immediate); env.allow_lease = false; ExpectPreserved(); CHECK(Calls("init") == 0);
    Reset(immediate); EmptyMedia(); ExpectComplete(); CHECK(env.random_calls == 1);
    for (size_t dirty : {size_t(0), size_t(1024), size_t(3072)}) {
        Reset(immediate); EmptyMedia(); env.flash[dirty] = 0x42; ExpectPreserved();
    }
    Reset(immediate); EmptyMedia(); env.data["other"]["kept"] = U32(1); ExpectPreserved();
    Reset(immediate); EmptyMedia(); env.data["wqn_boot"]["unknown"] = U32(1); ExpectPreserved();
    Reset(immediate); EmptyMedia(); env.has_partition = false; ExpectPreserved(ESP_ERR_NOT_FOUND);
    Reset(immediate); EmptyMedia(); env.partition.size = 0; ExpectPreserved(ESP_ERR_NOT_FOUND);
    Reset(immediate); EmptyMedia();
    env.data["legacy"]["blob"] = {NVS_TYPE_LEGACY, {9, 8, 7}}; ExpectPreserved();
    Reset(immediate); EmptyMedia(); Fail("stats"); ExpectPreserved(ESP_FAIL);
    for (unsigned read = 1; read <= 4; ++read) {
        Reset(immediate); EmptyMedia(); Fail("partition-read", read); ExpectPreserved(ESP_FAIL);
    }
    Reset(immediate); EmptyMedia(); Fail("find"); ExpectPreserved(ESP_FAIL);
    Reset(immediate); SetGeneration(0); ExpectComplete();
    Reset(immediate); SetIntent(0x69); env.mounted = true; ExpectComplete(0x69);
    CHECK(env.random_calls == 0);
    Reset(immediate); SetIntent(); env.data["wqn_boot"].erase("schema_gen"); ExpectComplete();
    Reset(immediate); const auto before_files = env.files;
    CHECK(InvalidateStorageSchemaForFactoryReset() == ESP_OK);
    CHECK(env.files == before_files && env.data["wqn_boot"]["schema_gen"] == U32(3));
    CHECK(HasIntent() && Calls("format") == 0 && Calls("set:reset_pending") == 1);
    CHECK(InvalidateStorageSchemaForFactoryReset() == ESP_OK);
    CHECK(Calls("set:reset_pending") == 1); Reboot(); ExpectComplete();
    for (uint32_t generation : {0u, 1u, 2u, 4u}) {
        Reset(immediate); SetGeneration(generation); const auto before = env.data;
        CHECK(InvalidateStorageSchemaForFactoryReset() == ESP_ERR_INVALID_STATE);
        CHECK(env.data == before && !HasIntent());
    }
    for (const char* fault : {"open-ro:wqn_boot", "get:schema_gen", "get-size:reset_id",
                             "get-data:reset_id", "get-size:reset_pending", "open-rw:wqn_boot",
                             "set:reset_pending", "commit:wqn_boot"}) {
        Reset(immediate); Fail(fault);
        CHECK(InvalidateStorageSchemaForFactoryReset() == ESP_FAIL);
        CHECK(env.files == before_files && env.handles.empty() && Calls("format") == 0);
        if (HasIntent()) { Reboot(); ExpectComplete(); }
    }
    // Fail each reset phase, then re-enter with persisted progress in both models.
    for (const auto& fault : std::vector<std::pair<std::string, unsigned>>{
        {"unmount", 1}, {"find", 1}, {"entry-info", 1}, {"next", 1},
        {"stats", 1}, {"used-count", 1},
        {"open-rw:audio", 1}, {"clear:audio", 1}, {"commit:audio", 1},
        {"clear:net", 1}, {"commit:net", 1}, {"clear:zz_wifi", 1},
        {"format", 1}, {"mount", 1}, {"info", 1}, {"unmount", 2},
        {"open-rw:wqn_boot", 1}, {"set:schema_gen", 1}, {"set:reset_id", 1},
        {"commit:wqn_boot", 1}, {"open-rw:wqn_boot", 2},
        {"erase:reset_pending", 1}, {"commit:wqn_boot", 2}}) {
        Reset(immediate); SetIntent(); env.mounted = true; Fail(fault.first, fault.second);
        result = Boot();
        CHECK(result.action == StorageSchemaBootAction::kRecoveryRequired && result.error == ESP_FAIL);
        if (fault.first == "set:reset_id" && immediate) {
            CHECK(env.data["wqn_boot"]["reset_id"].bytes == Bytes(16, 0xa5)); CHECK(HasIntent());
        }
        const bool pending = HasIntent();
        if (fault.first != "commit:wqn_boot" || fault.second != 2 || !immediate) CHECK(pending);
        Reboot();
        if (pending) ExpectComplete();
        else { result = Boot(); CHECK(result.action == StorageSchemaBootAction::kReady); }
    }
    for (const char* fault : {"clear:audio", "format", "set:schema_gen", "set:reset_id",
                             "erase:reset_pending"}) {
        Reset(immediate); SetIntent(); Fail(fault, 1, ESP_FAIL, true);
        result = Boot(); CHECK(result.action == StorageSchemaBootAction::kRecoveryRequired);
        const bool pending = HasIntent(); Reboot();
        if (pending) ExpectComplete();
        else { result = Boot(); CHECK(result.action == StorageSchemaBootAction::kReady); }
    }
    for (const char* fault : {"open-rw:wqn_boot", "set:reset_pending", "commit:wqn_boot"}) {
        Reset(immediate); EmptyMedia(); Fail(fault);
        result = Boot(); CHECK(result.action == StorageSchemaBootAction::kRecoveryRequired);
        CHECK(Calls("format") == 0);
    }
    Reset(immediate); SetIntent();
    env.data["legacy"]["blob"] = {NVS_TYPE_LEGACY, {9, 8, 7}};
    result = Boot(); CHECK(result.action == StorageSchemaBootAction::kRecoveryRequired);
    CHECK(result.error == ESP_ERR_INVALID_STATE && Calls("format") == 0 && HasIntent());
    CHECK(env.data["legacy"]["blob"].bytes == Bytes({9, 8, 7}));
    CHECK(env.files == original_files);
}
int main() {
    Run(false); Run(true);
    std::printf("PASS: boot schema %u checks in %u host scenarios (staged + immediate NVS); not HIL\n",
                checks, cases);
}
