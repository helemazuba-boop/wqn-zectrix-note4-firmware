#include "problem_pack.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#include "cJSON.h"
#include "device_protocol/problem_study.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "mbedtls/sha256.h"
#include "runtime/sleep_coordinator.h"
#include "services/storage_service.h"

namespace {

constexpr char kTag[] = "problem_pack";
constexpr char kStorageRoot[] = "/storage";
constexpr char kManifestPath[] = "/storage/pp_manifest.json";
constexpr char kManifestTempPath[] = "/storage/pp_manifest.tmp";
constexpr char kManifestBackupPath[] = "/storage/pp_manifest.bak";
constexpr uint64_t kPackSchemaVersion =
    wqn::protocol::problem_study_v1::kPackSchemaVersion;
constexpr size_t kMaxPackEntries = wqn::protocol::problem_study_v1::kMaxPackEntries;
// Packs are capped at 500 rows each; the global ceiling bounds the PSRAM
// index, which the LoadProblemPackIndex preflight then enforces exactly.
constexpr size_t kMaxIndexEntries = 4000;
constexpr size_t kMaxPackBytes = wqn::protocol::problem_study_v1::kMaxPackBytes;
constexpr size_t kMaxLineBytes = wqn::protocol::problem_study_v1::kMaxPackLineBytes;
constexpr size_t kMaxProblemParts = wqn::protocol::problem_study_v1::kMaxProblemParts;
constexpr size_t kMaxImagesPerRow = wqn::protocol::problem_study_v1::kMaxImagesPerRow;
// A device browses a bounded set of problem sets; cap the persisted aggregate
// so a runaway account cannot grow the manifest without limit.
constexpr size_t kMaxStoredSets = 200;
// Contract bounds: 200-char title / 200-char set name at 4 bytes per char.
constexpr size_t kMaxProblemTitleBytes = 4 * 200;
constexpr size_t kMaxSetNameBytes = 4 * 200;
// Defensive per-field caps; the 64 KB line bound dominates in practice.
constexpr size_t kMaxProblemContentBytes = 32768;
constexpr size_t kMaxPartTextBytes = 16384;
constexpr size_t kMaxPartLabelBytes = 80;
constexpr size_t kMaxPartTypeBytes = 32;
constexpr size_t kMaxPartAnswerBytes = 4096;
constexpr size_t kPackIdStemChars = 6;
constexpr size_t kPackHashStemChars = 12;
// SPIFFS counts the leading slash in its object name and reserves one byte for
// NUL. Keep the longest final suffix (.wqnp) strictly within that budget.
constexpr size_t kMaxPackObjectNameBytes =
    1 + 3 + kPackIdStemChars + 1 + kPackHashStemChars + 5;
static_assert(
    kMaxPackObjectNameBytes <= CONFIG_SPIFFS_OBJ_NAME_LEN - 1,
    "problem pack filename exceeds SPIFFS object-name budget");
// Maximum contract line, its optional LF, and the terminating NUL for fgets.
constexpr size_t kLineBufferSize = kMaxLineBytes + 2;

class JsonDocument {
public:
    explicit JsonDocument(const char* payload) : root_(cJSON_Parse(payload)) {}
    ~JsonDocument() { cJSON_Delete(root_); }

    cJSON* root() const { return root_; }
    bool ok() const { return root_ != nullptr; }

private:
    cJSON* root_ = nullptr;
};

std::string GetOptionalString(cJSON* object, const char* key)
{
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring != nullptr ? item->valuestring : "";
}

bool GetExactUint64(cJSON* object, const char* key, uint64_t* value)
{
    if (!cJSON_IsObject(object) || key == nullptr || value == nullptr) {
        return false;
    }
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
        item->valuedouble < 0 ||
        item->valuedouble > static_cast<double>(wqn::protocol::v3::kMaxSafeJsonInteger) ||
        std::floor(item->valuedouble) != item->valuedouble) {
        return false;
    }
    *value = static_cast<uint64_t>(item->valuedouble);
    return true;
}

bool FileExists(const std::string& path)
{
    struct stat st = {};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

void ReleaseProblemPackIndexAllocations(wqn::ProblemPackIndex* index)
{
    if (index == nullptr) return;
    // Swap with empty vectors so the PSRAM allocator releases capacity even when
    // index construction exits with pack_error (see note_pack.cpp).
    std::vector<
        wqn::ProblemPackIndexEntry,
        wqn::NotePsramAllocator<wqn::ProblemPackIndexEntry>> empty_entries;
    index->entries.swap(empty_entries);
    std::vector<uint32_t, wqn::NotePsramAllocator<uint32_t>> empty_order;
    index->problem_order.swap(empty_order);
}

// Reads one JSONL line. A problem pack body is `lines.join('\n')` with no
// trailing newline, so the final record is terminated by EOF; a missing LF
// that is NOT at EOF means the line overflowed the bounded buffer.
esp_err_t ReadBoundedProblemPackLine(FILE* file, std::vector<char>* buffer, std::string* line)
{
    if (file == nullptr || buffer == nullptr || line == nullptr ||
        buffer->size() != kLineBufferSize) {
        return ESP_ERR_INVALID_ARG;
    }
    line->clear();
    if (std::fgets(buffer->data(), buffer->size(), file) == nullptr) {
        return std::feof(file) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    const size_t length = std::strlen(buffer->data());
    const bool has_newline = length > 0 && (*buffer)[length - 1] == '\n';
    if (!has_newline && std::feof(file) == 0) {
        // fgets stopped because the buffer filled before any newline: overlong.
        return ESP_ERR_INVALID_SIZE;
    }
    size_t content_length = has_newline ? length - 1 : length;
    if (content_length > 0 && (*buffer)[content_length - 1] == '\r') {
        --content_length;
    }
    if (content_length > kMaxLineBytes) {
        return ESP_ERR_INVALID_SIZE;
    }
    line->assign(buffer->data(), content_length);
    return ESP_OK;
}

esp_err_t ReadWholeFile(const char* path, std::string* out)
{
    if (out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    out->clear();

    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    std::array<char, 512> buffer = {};
    while (true) {
        const size_t read = std::fread(buffer.data(), 1, buffer.size(), file);
        if (read > 0) {
            out->append(buffer.data(), read);
        }
        if (read < buffer.size()) {
            if (std::ferror(file)) {
                std::fclose(file);
                return ESP_FAIL;
            }
            break;
        }
    }
    std::fclose(file);
    return ESP_OK;
}

bool VerifyFileSha256(const std::string& path, const std::string& expected)
{
    if (expected.size() != 64) {
        return false;
    }
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    std::array<unsigned char, 1024> buffer = {};
    while (true) {
        const size_t read = std::fread(buffer.data(), 1, buffer.size(), file);
        if (read > 0) {
            mbedtls_sha256_update(&ctx, buffer.data(), read);
        }
        if (read < buffer.size()) {
            if (std::ferror(file)) {
                std::fclose(file);
                mbedtls_sha256_free(&ctx);
                return false;
            }
            break;
        }
    }
    std::fclose(file);

    std::array<unsigned char, 32> digest = {};
    mbedtls_sha256_finish(&ctx, digest.data());
    mbedtls_sha256_free(&ctx);
    constexpr char kHex[] = "0123456789abcdef";
    std::string actual;
    actual.reserve(64);
    for (const unsigned char byte : digest) {
        actual.push_back(kHex[byte >> 4]);
        actual.push_back(kHex[byte & 0x0F]);
    }
    return actual == expected;
}

void CopyField(char* dst, size_t dst_size, const std::string& src)
{
    if (dst == nullptr || dst_size == 0) {
        return;
    }
    const size_t n = std::min(src.size(), dst_size - 1);
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

// Copies a display title, truncating on a UTF-8 boundary so the list never
// shows a split multi-byte character.
void CopyTitleUtf8Safe(char* dst, size_t dst_size, const std::string& src)
{
    if (dst == nullptr || dst_size == 0) {
        return;
    }
    size_t n = std::min(src.size(), dst_size - 1);
    if (n < src.size()) {
        while (n > 0 && (static_cast<unsigned char>(src[n]) & 0xC0) == 0x80) {
            --n;
        }
    }
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

bool IsImageIdArrayValid(cJSON* array, size_t* count)
{
    if (count == nullptr || !cJSON_IsArray(array) ||
        cJSON_GetArraySize(array) > static_cast<int>(kMaxImagesPerRow)) {
        return false;
    }
    size_t seen = 0;
    cJSON* image_id = nullptr;
    cJSON_ArrayForEach(image_id, array) {
        if (!cJSON_IsString(image_id) || image_id->valuestring == nullptr ||
            std::strlen(image_id->valuestring) != 64) {
            return false;
        }
        ++seen;
    }
    *count = seen;
    return true;
}

}  // namespace

namespace wqn {

std::string SafeProblemPackStem(const WqnProblemPackManifestSet& set)
{
    std::string stem;
    stem.reserve(kPackIdStemChars + 1 + kPackHashStemChars);
    for (const char ch : set.pack_id) {
        const bool keep = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') ||
                          (ch >= 'A' && ch <= 'Z');
        if (keep) {
            stem.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
        if (stem.size() >= kPackIdStemChars) {
            break;
        }
    }
    if (stem.empty()) {
        stem = "prob";
    }
    if (!set.sha256.empty()) {
        stem.push_back('_');
        size_t hash_chars = 0;
        for (const char ch : set.sha256) {
            const bool keep = (ch >= '0' && ch <= '9') ||
                              (ch >= 'a' && ch <= 'f') ||
                              (ch >= 'A' && ch <= 'F');
            if (keep) {
                stem.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
                if (++hash_chars >= kPackHashStemChars) {
                    break;
                }
            }
        }
        if (hash_chars == 0) {
            stem.resize(stem.size() - 1);
        }
    }
    return stem;
}

}  // namespace wqn

namespace {

std::string PackPathForStem(const std::string& stem)
{
    return std::string(kStorageRoot) + "/pp_" + stem + ".wqnp";
}

std::string PackPathForSet(const wqn::WqnProblemPackManifestSet& set)
{
    return PackPathForStem(wqn::SafeProblemPackStem(set));
}

std::string TempPackPathForSet(const wqn::WqnProblemPackManifestSet& set)
{
    return std::string(kStorageRoot) + "/pp_" + wqn::SafeProblemPackStem(set) + ".tmp";
}

void AddManifestSetStems(
    const wqn::WqnProblemPackManifest& manifest,
    std::vector<std::string>* stems)
{
    if (stems == nullptr) {
        return;
    }
    for (const wqn::WqnProblemPackManifestSet& set : manifest.problem_sets) {
        if (!set.has_pack) {
            continue;
        }
        const std::string stem = wqn::SafeProblemPackStem(set);
        if (std::find(stems->begin(), stems->end(), stem) == stems->end()) {
            stems->push_back(stem);
        }
    }
}

esp_err_t ParseProblemManifestPayload(
    const std::string& payload, wqn::WqnProblemPackManifest* manifest)
{
    *manifest = {};
    JsonDocument document(payload.c_str());
    cJSON* data = document.ok()
        ? cJSON_GetObjectItemCaseSensitive(document.root(), "data")
        : nullptr;
    cJSON* has_more = data != nullptr
        ? cJSON_GetObjectItemCaseSensitive(data, "has_more")
        : nullptr;
    cJSON* sets = data != nullptr
        ? cJSON_GetObjectItemCaseSensitive(data, "problem_sets")
        : nullptr;
    if (!document.ok() || !cJSON_IsObject(data) ||
        !GetExactUint64(data, "cursor", &manifest->cursor) ||
        !cJSON_IsBool(has_more) || !cJSON_IsArray(sets)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    manifest->has_more = cJSON_IsTrue(has_more);

    const int count = cJSON_GetArraySize(sets);
    if (count < 0 || static_cast<size_t>(count) > kMaxStoredSets) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    manifest->problem_sets.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
        cJSON* object = cJSON_GetArrayItem(sets, index);
        wqn::WqnProblemPackManifestSet set;
        set.problem_set_id = GetOptionalString(object, "problem_set_id");
        set.name = GetOptionalString(object, "name");
        cJSON* is_smart = cJSON_GetObjectItemCaseSensitive(object, "is_smart");
        cJSON* has_pack = cJSON_GetObjectItemCaseSensitive(object, "has_pack");
        if (!cJSON_IsObject(object) || set.problem_set_id.size() != 36 ||
            set.name.empty() || set.name.size() > kMaxSetNameBytes ||
            !cJSON_IsBool(is_smart) || !cJSON_IsBool(has_pack)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        set.is_smart = cJSON_IsTrue(is_smart);
        set.has_pack = cJSON_IsTrue(has_pack);
        cJSON* pack = cJSON_GetObjectItemCaseSensitive(object, "pack");
        if (set.has_pack) {
            uint64_t schema_version = 0;
            uint64_t entry_count = 0;
            uint64_t byte_size = 0;
            set.pack_id = GetOptionalString(pack, "pack_id");
            set.sha256 = GetOptionalString(pack, "sha256");
            set.download_url = GetOptionalString(pack, "download_url");
            if (!cJSON_IsObject(pack) || set.pack_id.size() != 36 ||
                set.sha256.size() != 64 ||
                !GetExactUint64(pack, "pack_revision", &set.pack_revision) ||
                !GetExactUint64(pack, "schema_version", &schema_version) ||
                !GetExactUint64(pack, "entry_count", &entry_count) ||
                !GetExactUint64(pack, "byte_size", &byte_size) ||
                schema_version != kPackSchemaVersion ||
                entry_count > kMaxPackEntries || byte_size == 0 ||
                byte_size > kMaxPackBytes) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            set.schema_version = static_cast<uint32_t>(schema_version);
            set.entry_count = static_cast<uint32_t>(entry_count);
            set.byte_size = static_cast<uint32_t>(byte_size);
        }
        manifest->problem_sets.push_back(std::move(set));
    }
    return ESP_OK;
}

// Loads the persisted problem-pack manifest, falling back to the .bak copy if
// the primary file is missing or corrupt (mirrors the atomic save's rollback).
esp_err_t ParseStoredProblemManifest(wqn::WqnProblemPackManifest* manifest)
{
    if (manifest == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *manifest = {};
    std::string payload;
    esp_err_t result = ReadWholeFile(kManifestPath, &payload);
    if (result == ESP_OK) {
        result = ParseProblemManifestPayload(payload, manifest);
        if (result == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(kTag, "primary problem pack manifest invalid; trying backup");
    } else if (result != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(kTag, "primary problem pack manifest unreadable; trying backup");
    }

    payload.clear();
    const esp_err_t backup_result = ReadWholeFile(kManifestBackupPath, &payload);
    if (backup_result != ESP_OK) {
        return result == ESP_ERR_NOT_FOUND ? backup_result : result;
    }
    return ParseProblemManifestPayload(payload, manifest);
}

void PruneUnreferencedProblemPackFiles(const wqn::WqnProblemPackManifest& current)
{
    std::vector<std::string> retained_stems;
    AddManifestSetStems(current, &retained_stems);

    if (FileExists(kManifestBackupPath)) {
        wqn::WqnProblemPackManifest backup;
        std::string backup_payload;
        // Best-effort: keep files referenced by a readable backup so a rollback
        // still has its packs. A corrupt backup simply prunes more aggressively.
        if (ReadWholeFile(kManifestBackupPath, &backup_payload) == ESP_OK &&
            ParseProblemManifestPayload(backup_payload, &backup) == ESP_OK) {
            AddManifestSetStems(backup, &retained_stems);
        }
    }

    DIR* directory = opendir(kStorageRoot);
    if (directory == nullptr) {
        ESP_LOGW(kTag, "problem pack prune opendir failed");
        return;
    }
    while (dirent* entry = readdir(directory)) {
        const std::string name = entry->d_name;
        constexpr char kPrefix[] = "pp_";
        constexpr char kSuffix[] = ".wqnp";
        if (name.size() <= sizeof(kPrefix) - 1 + sizeof(kSuffix) - 1 ||
            name.compare(0, sizeof(kPrefix) - 1, kPrefix) != 0 ||
            name.compare(name.size() - (sizeof(kSuffix) - 1), sizeof(kSuffix) - 1, kSuffix) != 0) {
            continue;
        }
        const std::string stem = name.substr(
            sizeof(kPrefix) - 1,
            name.size() - (sizeof(kPrefix) - 1) - (sizeof(kSuffix) - 1));
        if (std::find(retained_stems.begin(), retained_stems.end(), stem) != retained_stems.end()) {
            continue;
        }
        const std::string path = std::string(kStorageRoot) + "/" + name;
        if (std::remove(path.c_str()) == 0) {
            ESP_LOGI(kTag, "pruned stale problem pack: %s", name.c_str());
        } else {
            ESP_LOGW(kTag, "failed to prune stale problem pack: %s", name.c_str());
        }
    }
    closedir(directory);
}

esp_err_t ResetProblemPackStorageCacheRaw(void*)
{
    const char* manifest_paths[] = {
        kManifestPath,
        kManifestTempPath,
        kManifestBackupPath,
    };
    for (const char* path : manifest_paths) {
        if (std::remove(path) != 0 && errno != ENOENT) {
            return ESP_FAIL;
        }
    }

    DIR* directory = opendir(kStorageRoot);
    if (directory == nullptr) {
        return ESP_FAIL;
    }
    esp_err_t result = ESP_OK;
    while (dirent* entry = readdir(directory)) {
        const std::string name = entry->d_name;
        const bool managed_pack =
            name.size() > 8 && name.compare(0, 3, "pp_") == 0 &&
            (name.compare(name.size() - 5, 5, ".wqnp") == 0 ||
             name.compare(name.size() - 4, 4, ".tmp") == 0);
        if (!managed_pack) {
            continue;
        }
        const std::string path = std::string(kStorageRoot) + "/" + name;
        if (std::remove(path.c_str()) != 0 && errno != ENOENT) {
            result = ESP_FAIL;
            ESP_LOGW(kTag, "failed to clear problem pack cache file: %s", name.c_str());
        }
    }
    closedir(directory);
    return result;
}

esp_err_t ScanProblemPackFile(
    const wqn::WqnProblemPackManifestSet& set,
    uint32_t set_order,
    wqn::ProblemPackIndex* index)
{
    if (index == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const std::string path = PackPathForSet(set);
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }
    const size_t initial_entry_count = index->entries.size();
    struct EntryRollback {
        wqn::ProblemPackIndex* index;
        size_t initial_size;
        bool committed = false;
        ~EntryRollback()
        {
            if (!committed && index != nullptr) {
                index->entries.resize(initial_size);
            }
        }
    } rollback{index, initial_entry_count};

    // [stack-fix] Keep the ~64 KB line buffer off the 8 KB task stack.
    std::vector<char> line_buffer(kLineBufferSize, 0);
    std::string line;
    // Line 1 is the metadata record (there is no magic header).
    esp_err_t result = ReadBoundedProblemPackLine(file, &line_buffer, &line);
    if (result != ESP_OK) {
        std::fclose(file);
        return result;
    }
    JsonDocument metadata(line.c_str());
    uint64_t metadata_schema = 0;
    uint64_t metadata_revision = 0;
    uint64_t metadata_count = 0;
    if (!metadata.ok() || !cJSON_IsObject(metadata.root()) ||
        !GetExactUint64(metadata.root(), "v", &metadata_schema) ||
        metadata_schema != kPackSchemaVersion ||
        GetOptionalString(metadata.root(), "problem_set_id") != set.problem_set_id ||
        !GetExactUint64(metadata.root(), "pack_revision", &metadata_revision) ||
        metadata_revision != set.pack_revision ||
        !GetExactUint64(metadata.root(), "count", &metadata_count) ||
        metadata_count != set.entry_count) {
        std::fclose(file);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (set.entry_count > 0) {
        const size_t target = index->entries.size() + static_cast<size_t>(set.entry_count);
        if (target > kMaxIndexEntries) {
            std::fclose(file);
            return ESP_ERR_INVALID_SIZE;
        }
        if (target > index->entries.capacity()) {
            index->entries.reserve(target);
        }
    }

    const std::string pack_stem = wqn::SafeProblemPackStem(set);
    uint32_t scanned_entries = 0;
    while (true) {
        const long offset = std::ftell(file);
        result = ReadBoundedProblemPackLine(file, &line_buffer, &line);
        if (result == ESP_ERR_NOT_FOUND) {
            break;
        }
        if (result != ESP_OK || offset < 0 ||
            static_cast<unsigned long>(offset) > std::numeric_limits<uint32_t>::max()) {
            std::fclose(file);
            return result == ESP_OK ? ESP_ERR_INVALID_SIZE : result;
        }
        wqn::WqnProblemEntry entry;
        result = wqn::ParseProblemRecordLine(line.c_str(), &entry, /*include_content=*/false);
        if (result != ESP_OK) {
            std::fclose(file);
            return result;
        }
        wqn::ProblemPackIndexEntry indexed = {};
        CopyField(indexed.problem_id, sizeof(indexed.problem_id), entry.problem_id);
        CopyField(indexed.set_id, sizeof(indexed.set_id), set.problem_set_id);
        CopyField(indexed.pack_stem, sizeof(indexed.pack_stem), pack_stem);
        CopyTitleUtf8Safe(indexed.title, sizeof(indexed.title), entry.title);
        indexed.file_offset = static_cast<uint32_t>(offset);
        indexed.set_order = set_order;
        indexed.image_count = static_cast<uint8_t>(entry.image_ids.size());
        indexed.solution_image_count =
            static_cast<uint8_t>(entry.solution_image_ids.size());
        indexed.status = static_cast<uint8_t>(entry.status);
        index->entries.push_back(indexed);
        ++scanned_entries;
        if (index->entries.size() > kMaxIndexEntries || scanned_entries > set.entry_count) {
            std::fclose(file);
            return ESP_ERR_INVALID_SIZE;
        }
    }
    std::fclose(file);
    if (scanned_entries != set.entry_count) {
        return ESP_ERR_INVALID_SIZE;
    }
    rollback.committed = true;
    return ESP_OK;
}

}  // namespace

namespace wqn {

esp_err_t InitProblemPackStorage()
{
    size_t total = 0;
    size_t used = 0;
    const esp_err_t info_result = esp_spiffs_info("storage", &total, &used);
    if (info_result != ESP_OK) {
        ESP_LOGW(kTag, "storage SPIFFS not mounted: %s", esp_err_to_name(info_result));
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t ResetProblemPackStorageCache()
{
    runtime::SleepLease storage_lease = runtime::SleepLease::TryAcquire(
        runtime::SleepBlocker::kStorage,
        "problem-pack-cache-reset",
        __FILE__,
        __LINE__);
    if (!storage_lease) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t result = services::ExecuteStorageTransaction(
        ResetProblemPackStorageCacheRaw, nullptr);
    if (result == ESP_OK) {
        ESP_LOGW(kTag, "cleared incompatible problem pack cache; cloud content is recoverable");
    }
    return result;
}

esp_err_t LoadProblemPackManifest(WqnProblemPackManifest* manifest)
{
    if (manifest == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *manifest = {};
    return ParseStoredProblemManifest(manifest);
}

esp_err_t MergeProblemPackManifestDelta(
    const WqnProblemPackManifest& delta,
    WqnProblemPackManifest* merged)
{
    if (merged == nullptr || delta.cursor > protocol::v3::kMaxSafeJsonInteger) {
        return ESP_ERR_INVALID_ARG;
    }
    WqnProblemPackManifest current;
    const esp_err_t load_result = LoadProblemPackManifest(&current);
    if (load_result != ESP_OK && load_result != ESP_ERR_NOT_FOUND) {
        return load_result;
    }
    if (load_result == ESP_ERR_NOT_FOUND) {
        current = {};
    }
    // No monotonic guard on the cursor: it is a set-list offset (the sync
    // relists from 0 every cycle), not a change feed, so it legitimately moves
    // backwards between syncs and shrinks when sets are deleted.

    for (const WqnProblemPackManifestSet& change : delta.problem_sets) {
        auto existing = std::find_if(
            current.problem_sets.begin(),
            current.problem_sets.end(),
            [&](const WqnProblemPackManifestSet& item) {
                return item.problem_set_id == change.problem_set_id;
            });
        if (change.deleted) {
            if (existing != current.problem_sets.end()) {
                current.problem_sets.erase(existing);
            }
            continue;
        }
        if (change.problem_set_id.size() != 36 ||
            (change.has_pack && change.pack_id.size() != 36)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (existing == current.problem_sets.end()) {
            current.problem_sets.push_back(change);
        } else {
            *existing = change;
        }
    }
    if (current.problem_sets.size() > kMaxStoredSets) {
        return ESP_ERR_INVALID_SIZE;
    }
    std::sort(
        current.problem_sets.begin(),
        current.problem_sets.end(),
        [](const WqnProblemPackManifestSet& left, const WqnProblemPackManifestSet& right) {
            return left.problem_set_id < right.problem_set_id;
        });
    current.cursor = delta.cursor;
    current.has_more = delta.has_more;
    *merged = std::move(current);
    return ESP_OK;
}

esp_err_t SaveProblemPackManifestRaw(const WqnProblemPackManifest& manifest)
{
    cJSON* root = cJSON_CreateObject();
    cJSON* data = cJSON_CreateObject();
    cJSON* sets = cJSON_CreateArray();
    if (root == nullptr || data == nullptr || sets == nullptr) {
        cJSON_Delete(root);
        cJSON_Delete(data);
        cJSON_Delete(sets);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddItemToObject(root, "data", data);
    cJSON_AddNumberToObject(data, "cursor", static_cast<double>(manifest.cursor));
    cJSON_AddBoolToObject(data, "has_more", manifest.has_more);
    cJSON_AddItemToObject(data, "problem_sets", sets);

    for (const WqnProblemPackManifestSet& item : manifest.problem_sets) {
        cJSON* set = cJSON_CreateObject();
        if (set == nullptr) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToArray(sets, set);
        cJSON_AddStringToObject(set, "problem_set_id", item.problem_set_id.c_str());
        cJSON_AddStringToObject(set, "name", item.name.c_str());
        cJSON_AddBoolToObject(set, "is_smart", item.is_smart);
        cJSON_AddBoolToObject(set, "has_pack", item.has_pack);
        if (!item.has_pack) {
            cJSON_AddNullToObject(set, "pack");
            continue;
        }
        cJSON* pack = cJSON_CreateObject();
        if (pack == nullptr) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToObject(set, "pack", pack);
        cJSON_AddStringToObject(pack, "pack_id", item.pack_id.c_str());
        cJSON_AddNumberToObject(pack, "pack_revision", static_cast<double>(item.pack_revision));
        cJSON_AddNumberToObject(pack, "schema_version", item.schema_version);
        cJSON_AddNumberToObject(pack, "entry_count", item.entry_count);
        cJSON_AddNumberToObject(pack, "byte_size", item.byte_size);
        cJSON_AddStringToObject(pack, "sha256", item.sha256.c_str());
        cJSON_AddStringToObject(pack, "download_url", item.download_url.c_str());
    }

    char* rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (rendered == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    FILE* file = std::fopen(kManifestTempPath, "wb");
    if (file == nullptr) {
        ESP_LOGW(kTag, "problem pack manifest save fopen failed: %s", kManifestTempPath);
        cJSON_free(rendered);
        return ESP_FAIL;
    }
    const size_t length = std::strlen(rendered);
    const size_t written = std::fwrite(rendered, 1, length, file);
    const bool flushed = std::fflush(file) == 0 && ::fsync(fileno(file)) == 0;
    const bool closed = std::fclose(file) == 0;
    cJSON_free(rendered);
    if (written != length || !flushed || !closed) {
        ESP_LOGW(kTag, "problem pack manifest durable write failed: want=%u got=%u",
                 static_cast<unsigned>(length), static_cast<unsigned>(written));
        std::remove(kManifestTempPath);
        return ESP_FAIL;
    }

    const bool had_primary = FileExists(kManifestPath);
    if (had_primary) {
        if (std::remove(kManifestBackupPath) != 0 && errno != ENOENT) {
            ESP_LOGW(kTag, "problem pack manifest stale backup remove failed");
            std::remove(kManifestTempPath);
            return ESP_FAIL;
        }
        if (std::rename(kManifestPath, kManifestBackupPath) != 0) {
            ESP_LOGW(kTag, "problem pack manifest backup commit failed");
            std::remove(kManifestTempPath);
            return ESP_FAIL;
        }
    }
    if (std::rename(kManifestTempPath, kManifestPath) != 0) {
        ESP_LOGW(kTag, "problem pack manifest commit rename failed");
        if (had_primary && std::rename(kManifestBackupPath, kManifestPath) != 0) {
            ESP_LOGE(kTag, "problem pack manifest rollback rename failed; backup remains readable");
        }
        std::remove(kManifestTempPath);
        return ESP_FAIL;
    }
    PruneUnreferencedProblemPackFiles(manifest);
    return ESP_OK;
}

esp_err_t SaveProblemPackManifestTransaction(void* opaque)
{
    return SaveProblemPackManifestRaw(
        *static_cast<const WqnProblemPackManifest*>(opaque));
}

esp_err_t SaveProblemPackManifest(const WqnProblemPackManifest& manifest)
{
    runtime::SleepLease storage_lease = runtime::SleepLease::TryAcquire(
        runtime::SleepBlocker::kStorage, "problem-pack-manifest", __FILE__, __LINE__);
    if (!storage_lease) {
        return ESP_ERR_INVALID_STATE;
    }
    return services::ExecuteStorageTransaction(
        SaveProblemPackManifestTransaction,
        const_cast<WqnProblemPackManifest*>(&manifest));
}

esp_err_t ParseProblemRecordLine(const char* line, WqnProblemEntry* entry, bool include_content)
{
    if (line == nullptr || entry == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *entry = {};
    JsonDocument document(line);
    if (!document.ok() || !cJSON_IsObject(document.root())) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    entry->problem_id = GetOptionalString(document.root(), "problem_id");
    entry->title = GetOptionalString(document.root(), "title");
    if (entry->problem_id.size() != 36 || entry->title.empty() ||
        entry->title.size() > kMaxProblemTitleBytes) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!protocol::problem_study_v1::ParseProblemStatus(
            GetOptionalString(document.root(), "status"), &entry->status)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON* is_optional = cJSON_GetObjectItemCaseSensitive(document.root(), "is_optional");
    if (!cJSON_IsBool(is_optional)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    entry->is_optional = cJSON_IsTrue(is_optional);

    // Both image arrays are always present in the contract; ids are needed
    // by the index (counts) and the viewer (fetch), so materialise them.
    size_t image_count = 0;
    size_t solution_image_count = 0;
    cJSON* image_ids = cJSON_GetObjectItemCaseSensitive(document.root(), "image_ids");
    cJSON* solution_image_ids =
        cJSON_GetObjectItemCaseSensitive(document.root(), "solution_image_ids");
    if (!IsImageIdArrayValid(image_ids, &image_count) ||
        !IsImageIdArrayValid(solution_image_ids, &solution_image_count)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON* image_id = nullptr;
    cJSON_ArrayForEach(image_id, image_ids) {
        entry->image_ids.emplace_back(image_id->valuestring);
    }
    cJSON_ArrayForEach(image_id, solution_image_ids) {
        entry->solution_image_ids.emplace_back(image_id->valuestring);
    }

    // parts must be a well-formed 1..10 array even on index scans; the
    // per-part text is only materialised (and field-validated) on a body
    // read so scans stay cheap.
    cJSON* parts = cJSON_GetObjectItemCaseSensitive(document.root(), "parts");
    const int part_count = cJSON_GetArraySize(parts);
    if (!cJSON_IsArray(parts) || part_count < 1 ||
        static_cast<size_t>(part_count) > kMaxProblemParts) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!include_content) {
        return ESP_OK;
    }

    // content_text may legitimately be empty (image-only problems).
    entry->content_text = GetOptionalString(document.root(), "content_text");
    if (entry->content_text.size() > kMaxProblemContentBytes) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    entry->parts.reserve(static_cast<size_t>(part_count));
    cJSON* part_object = nullptr;
    cJSON_ArrayForEach(part_object, parts) {
        uint64_t part_index = 0;
        uint64_t full_marks = 0;
        WqnProblemPackPart part;
        part.label = GetOptionalString(part_object, "label");
        part.type = GetOptionalString(part_object, "type");
        part.content_text = GetOptionalString(part_object, "content_text");
        part.answer_text = GetOptionalString(part_object, "answer_text");
        if (!cJSON_IsObject(part_object) ||
            !GetExactUint64(part_object, "index", &part_index) ||
            part_index < 1 || part_index > kMaxProblemParts ||
            !GetExactUint64(part_object, "full_marks", &full_marks) ||
            full_marks > 200 ||
            part.label.size() > kMaxPartLabelBytes ||
            part.type.empty() || part.type.size() > kMaxPartTypeBytes ||
            part.content_text.size() > kMaxPartTextBytes ||
            part.answer_text.size() > kMaxPartAnswerBytes) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        part.index = static_cast<int>(part_index);
        part.full_marks = static_cast<int>(full_marks);
        entry->parts.push_back(std::move(part));
    }
    return ESP_OK;
}

esp_err_t LoadProblemPackIndex(ProblemPackIndex* index)
{
    if (index == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    // SHA verification and JSONL scanning are CPU-bound; scope max frequency
    // to this rebuild instead of disabling dynamic frequency scaling globally.
    auto cpu_lease = runtime::CpuPerformanceLease::TryAcquire();
    *index = ProblemPackIndex{};
    index->mounted = InitProblemPackStorage() == ESP_OK;
    if (!index->mounted) {
        index->status_message = "错题分区不可用";
        return ESP_OK;
    }

    WqnProblemPackManifest manifest;
    const esp_err_t manifest_result = LoadProblemPackManifest(&manifest);
    if (manifest_result == ESP_ERR_NOT_FOUND) {
        index->status_message = "错题未同步";
        return ESP_OK;
    }
    if (manifest_result != ESP_OK) {
        index->pack_error = true;
        index->status_message = "错题清单损坏";
        return ESP_OK;
    }

    index->has_manifest = true;
    index->set_count = manifest.problem_sets.size();

    size_t expected_entries = 0;
    for (const WqnProblemPackManifestSet& item : manifest.problem_sets) {
        if (item.problem_set_id.size() != 36 ||
            (item.has_pack && item.sha256.size() != 64)) {
            index->pack_error = true;
            index->status_message = "错题清单无效";
            ReleaseProblemPackIndexAllocations(index);
            return ESP_OK;
        }
        if (!item.has_pack) {
            continue;
        }
        if (item.entry_count > kMaxIndexEntries - expected_entries) {
            index->pack_error = true;
            index->status_message = "错题条目过多";
            ReleaseProblemPackIndexAllocations(index);
            return ESP_OK;
        }
        expected_entries += item.entry_count;
    }

    const size_t entry_index_bytes = expected_entries * sizeof(ProblemPackIndexEntry);
    constexpr size_t kIndexAllocationReserveBytes = 64U * 1024U;
    const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t largest_psram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    if (entry_index_bytes > free_psram ||
        free_psram - entry_index_bytes < kIndexAllocationReserveBytes ||
        entry_index_bytes > largest_psram) {
        index->pack_error = true;
        index->status_message = "错题索引内存不足";
        ESP_LOGW(
            kTag,
            "problem pack index PSRAM preflight failed: need=%u free=%u largest=%u",
            static_cast<unsigned>(entry_index_bytes),
            static_cast<unsigned>(free_psram),
            static_cast<unsigned>(largest_psram));
        ReleaseProblemPackIndexAllocations(index);
        return ESP_OK;
    }
    index->entries.reserve(expected_entries);
    index->sets.reserve(manifest.problem_sets.size());

    for (const WqnProblemPackManifestSet& item : manifest.problem_sets) {
        const uint32_t set_order = static_cast<uint32_t>(index->sets.size());
        ProblemPackSet set;
        set.set_id = item.problem_set_id;
        set.name = item.name;
        set.is_smart = item.is_smart;
        set.pack_revision = item.pack_revision;
        set.sha256 = item.sha256;
        set.has_pack = item.has_pack;
        set.entry_begin = index->entries.size();
        set.entry_count = 0;

        if (item.has_pack) {
            const std::string path = PackPathForSet(item);
            struct stat st = {};
            if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                index->pack_bytes += static_cast<size_t>(st.st_size);
            }
            if (!VerifyFileSha256(path, item.sha256)) {
                index->pack_error = true;
                index->status_message = "错题校验失败";
            } else {
                const esp_err_t scan_result = ScanProblemPackFile(item, set_order, index);
                if (scan_result != ESP_OK) {
                    index->pack_error = true;
                    index->status_message = "错题读取失败";
                } else {
                    set.entry_count = index->entries.size() - set.entry_begin;
                }
            }
        }
        index->sets.push_back(std::move(set));
    }

    // Build the problem_id-sorted lookup once, after every set is scanned.
    // entries stay in display order; problem_order holds indices sorted by
    // problem_id so a lookup can binary-search instead of scanning O(N).
    index->problem_order.reserve(index->entries.size());
    for (size_t i = 0; i < index->entries.size(); ++i) {
        index->problem_order.push_back(static_cast<uint32_t>(i));
    }
    std::sort(
        index->problem_order.begin(), index->problem_order.end(),
        [&index](uint32_t a, uint32_t b) {
            return std::strcmp(index->entries[a].problem_id, index->entries[b].problem_id) < 0;
        });

    if (index->sets.empty() && index->status_message.empty()) {
        index->status_message = "暂无错题本";
    } else if (index->status_message.empty()) {
        index->status_message = "错题已就绪";
    }

    ESP_LOGI(
        kTag,
        "problem pack index: sets=%u problems=%u pack_bytes=%u free_internal=%u free_psram=%u",
        static_cast<unsigned>(index->set_count),
        static_cast<unsigned>(index->entries.size()),
        static_cast<unsigned>(index->pack_bytes),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    return ESP_OK;
}

enum class ProblemPackStreamOperation : uint8_t {
    kBegin,
    kAppend,
    kCommit,
    kAbort,
};

struct ProblemPackStreamContext {
    const WqnProblemPackManifestSet* set = nullptr;
    FILE* file = nullptr;
    mbedtls_sha256_context sha = {};
    bool sha_started = false;
    size_t bytes_written = 0;
    const uint8_t* chunk = nullptr;
    size_t chunk_size = 0;
    ProblemPackStreamOperation operation = ProblemPackStreamOperation::kBegin;
};

esp_err_t ProblemPackStreamTransaction(void* opaque)
{
    auto* context = static_cast<ProblemPackStreamContext*>(opaque);
    if (context == nullptr || context->set == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const std::string temp_path = TempPackPathForSet(*context->set);
    const std::string final_path = PackPathForSet(*context->set);
    switch (context->operation) {
        case ProblemPackStreamOperation::kBegin:
            std::remove(temp_path.c_str());
            context->file = std::fopen(temp_path.c_str(), "wb");
            if (context->file == nullptr) {
                ESP_LOGW(kTag, "problem pack temp open failed: path=%s errno=%d", temp_path.c_str(), errno);
                return ESP_FAIL;
            }
            if (mbedtls_sha256_starts(&context->sha, 0) != 0) {
                std::fclose(context->file);
                context->file = nullptr;
                std::remove(temp_path.c_str());
                return ESP_FAIL;
            }
            context->sha_started = true;
            context->bytes_written = 0;
            return ESP_OK;

        case ProblemPackStreamOperation::kAppend:
            if (context->file == nullptr || !context->sha_started ||
                context->chunk == nullptr || context->chunk_size == 0 ||
                context->bytes_written + context->chunk_size > context->set->byte_size ||
                context->bytes_written + context->chunk_size > kMaxPackBytes) {
                return ESP_ERR_INVALID_SIZE;
            }
            if (std::fwrite(context->chunk, 1, context->chunk_size, context->file) !=
                context->chunk_size) {
                return ESP_FAIL;
            }
            if (mbedtls_sha256_update(&context->sha, context->chunk, context->chunk_size) != 0) {
                return ESP_FAIL;
            }
            context->bytes_written += context->chunk_size;
            return ESP_OK;

        case ProblemPackStreamOperation::kCommit: {
            if (context->file == nullptr || !context->sha_started ||
                context->bytes_written != context->set->byte_size) {
                return ESP_ERR_INVALID_SIZE;
            }
            std::array<unsigned char, 32> digest = {};
            if (mbedtls_sha256_finish(&context->sha, digest.data()) != 0) {
                return ESP_FAIL;
            }
            context->sha_started = false;
            constexpr char kHex[] = "0123456789abcdef";
            std::string actual_sha;
            actual_sha.reserve(64);
            for (const unsigned char byte : digest) {
                actual_sha.push_back(kHex[byte >> 4]);
                actual_sha.push_back(kHex[byte & 0x0f]);
            }
            if (actual_sha != context->set->sha256) {
                std::fclose(context->file);
                context->file = nullptr;
                std::remove(temp_path.c_str());
                return ESP_ERR_INVALID_CRC;
            }
            const bool flushed = std::fflush(context->file) == 0 &&
                ::fsync(fileno(context->file)) == 0;
            const bool closed = std::fclose(context->file) == 0;
            context->file = nullptr;
            if (!flushed || !closed) {
                std::remove(temp_path.c_str());
                return ESP_FAIL;
            }
            if (FileExists(final_path) && std::remove(final_path.c_str()) != 0) {
                std::remove(temp_path.c_str());
                return ESP_FAIL;
            }
            if (std::rename(temp_path.c_str(), final_path.c_str()) != 0) {
                std::remove(temp_path.c_str());
                return ESP_FAIL;
            }
            return ESP_OK;
        }

        case ProblemPackStreamOperation::kAbort:
            if (context->file != nullptr) {
                std::fclose(context->file);
                context->file = nullptr;
            }
            std::remove(temp_path.c_str());
            return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t AppendProblemPackStream(void* opaque, const uint8_t* bytes, size_t size)
{
    auto* context = static_cast<ProblemPackStreamContext*>(opaque);
    if (context == nullptr || bytes == nullptr || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    context->operation = ProblemPackStreamOperation::kAppend;
    context->chunk = bytes;
    context->chunk_size = size;
    return services::ExecuteStorageTransaction(ProblemPackStreamTransaction, context);
}

esp_err_t DownloadProblemPackToStorage(
    const std::string& token,
    const protocol::v3::RequestMetadata& metadata,
    const WqnProblemPackManifestSet& set)
{
    if (!set.has_pack || set.pack_id.size() != 36 ||
        set.sha256.size() != 64 ||
        set.schema_version != kPackSchemaVersion ||
        set.entry_count > kMaxPackEntries || set.byte_size == 0 ||
        set.byte_size > kMaxPackBytes) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ProblemPackNeedsDownload(set)) {
        return ESP_OK;
    }
    runtime::SleepLease storage_lease = runtime::SleepLease::TryAcquire(
        runtime::SleepBlocker::kStorage, "problem-pack-stream", __FILE__, __LINE__);
    if (!storage_lease) {
        return ESP_ERR_INVALID_STATE;
    }
    ProblemPackStreamContext context;
    context.set = &set;
    mbedtls_sha256_init(&context.sha);
    context.operation = ProblemPackStreamOperation::kBegin;
    esp_err_t result = services::ExecuteStorageTransaction(
        ProblemPackStreamTransaction, &context);
    if (result == ESP_OK) {
        result = DownloadProblemPackStream(
            token, metadata, set, AppendProblemPackStream, &context);
    }
    if (result == ESP_OK) {
        context.operation = ProblemPackStreamOperation::kCommit;
        result = services::ExecuteStorageTransaction(
            ProblemPackStreamTransaction, &context);
    }
    if (result != ESP_OK) {
        context.operation = ProblemPackStreamOperation::kAbort;
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            services::ExecuteStorageTransaction(ProblemPackStreamTransaction, &context));
    }
    mbedtls_sha256_free(&context.sha);
    return result;
}

bool ProblemPackNeedsDownload(
    const WqnProblemPackManifestSet& set,
    const std::string* verified_sha)
{
    if (!set.has_pack) {
        return false;
    }
    const std::string path = PackPathForSet(set);
    struct stat status = {};
    if (stat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode)) {
        return true;
    }
    if (verified_sha != nullptr && *verified_sha == set.sha256 &&
        static_cast<uint64_t>(status.st_size) == set.byte_size) {
        // The listing matches the sha we verified at download time and the
        // file is size-intact: skip the full-file re-hash.
        return false;
    }
    return !VerifyFileSha256(path, set.sha256);
}

esp_err_t ReadProblemPackEntry(
    const ProblemPackIndexEntry& index_entry, WqnProblemEntry* entry)
{
    if (entry == nullptr || index_entry.pack_stem[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    // A problem open performs a bounded SPIFFS seek, JSONL read and cJSON
    // parse; keep it off the 40 MHz DFS floor as well.
    auto cpu_lease = runtime::CpuPerformanceLease::TryAcquire();
    *entry = WqnProblemEntry{};

    const std::string path = PackPathForStem(index_entry.pack_stem);
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }
    if (std::fseek(file, static_cast<long>(index_entry.file_offset), SEEK_SET) != 0) {
        std::fclose(file);
        return ESP_FAIL;
    }
    // [stack-fix] Keep the ~64 KB line buffer off the task stack.
    std::vector<char> line_buffer(kLineBufferSize, 0);
    std::string line;
    const esp_err_t line_result = ReadBoundedProblemPackLine(file, &line_buffer, &line);
    std::fclose(file);
    if (line_result != ESP_OK) {
        return line_result;
    }
    const esp_err_t parse_result = ParseProblemRecordLine(line.c_str(), entry, /*include_content=*/true);
    if (parse_result != ESP_OK) {
        return parse_result;
    }
    if (entry->problem_id != index_entry.problem_id) {
        // The index and the file disagree: a stale index after packs changed.
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

}  // namespace wqn
