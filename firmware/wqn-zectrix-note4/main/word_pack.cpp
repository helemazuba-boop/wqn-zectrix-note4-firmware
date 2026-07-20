#include "word_pack.h"

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
#include "device_protocol/word_study.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "mbedtls/sha256.h"
#include "runtime/sleep_coordinator.h"
#include "services/storage_service.h"
#include "word_study_store.h"

namespace {

constexpr char kTag[] = "word_pack";
constexpr char kStorageRoot[] = "/storage";
constexpr char kManifestPath[] = "/storage/wp_manifest.json";
constexpr char kManifestTempPath[] = "/storage/wp_manifest.tmp";
constexpr char kManifestBackupPath[] = "/storage/wp_manifest.bak";
constexpr char kPackMagic[] = "WQN_WORD_PACK_V2";
constexpr size_t kMaxIndexEntries = wqn::protocol::word_study_v1::kMaxPackEntries;
constexpr size_t kMaxPackBytes = wqn::protocol::word_study_v1::kMaxPackBytes;
constexpr size_t kMaxLineBytes = wqn::protocol::word_study_v1::kMaxPackLineBytes;
constexpr size_t kPackIdStemChars = 6;
constexpr size_t kPackHashStemChars = 12;
constexpr wqn::protocol::word_study_v1::Mode kPersistedSessionModes[] = {
    wqn::protocol::word_study_v1::Mode::kSequential,
    wqn::protocol::word_study_v1::Mode::kRandom,
    wqn::protocol::word_study_v1::Mode::kDictionary,
};
// SPIFFS counts the leading slash in its object name and reserves one byte for
// NUL. Keep the longest final suffix (.wqwp) strictly within that budget.
constexpr size_t kMaxPackObjectNameBytes =
    1 + 3 + kPackIdStemChars + 1 + kPackHashStemChars + 5;
static_assert(
    kMaxPackObjectNameBytes <= CONFIG_SPIFFS_OBJ_NAME_LEN - 1,
    "word pack filename exceeds SPIFFS object-name budget");
// Maximum contract line, its required LF, and the terminating NUL for fgets.
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

bool GetExactInt32(cJSON* object, const char* key, int32_t* value)
{
    if (!cJSON_IsObject(object) || key == nullptr || value == nullptr) {
        return false;
    }
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
        std::floor(item->valuedouble) != item->valuedouble ||
        item->valuedouble < std::numeric_limits<int32_t>::min() ||
        item->valuedouble > std::numeric_limits<int32_t>::max()) {
        return false;
    }
    *value = static_cast<int32_t>(item->valuedouble);
    return true;
}

bool FileExists(const std::string& path)
{
    struct stat st = {};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

esp_err_t ReadBoundedPackLine(FILE* file, std::vector<char>* buffer, std::string* line)
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
    if (length == 0 || (*buffer)[length - 1] != '\n') {
        // Every v2 line is LF-terminated. Missing LF also detects an overlong
        // line before cJSON sees a truncated prefix.
        return ESP_ERR_INVALID_SIZE;
    }
    size_t content_length = length - 1;
    if (content_length > 0 && (*buffer)[content_length - 1] == '\r') {
        --content_length;
    }
    if (content_length > kMaxLineBytes) {
        return ESP_ERR_INVALID_SIZE;
    }
    line->assign(buffer->data(), content_length);
    return ESP_OK;
}

}  // namespace

namespace wqn {

std::string SafePackStem(const WqnWordPackManifestItem& item)
{
    std::string stem;
    stem.reserve(kPackIdStemChars + 1 + kPackHashStemChars);
    for (const char ch : item.pack_id) {
        const bool keep = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        if (keep) {
            stem.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
        if (stem.size() >= kPackIdStemChars) {
            break;
        }
    }
    if (stem.empty()) {
        stem = "pack";
    }
    if (!item.sha256.empty()) {
        stem.push_back('_');
        size_t hash_chars = 0;
        for (const char ch : item.sha256) {
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
    return std::string(kStorageRoot) + "/wp_" + stem + ".wqwp";
}

std::string PackPathForItem(const wqn::WqnWordPackManifestItem& item)
{
    return PackPathForStem(wqn::SafePackStem(item));
}

std::string TempPackPathForItem(const wqn::WqnWordPackManifestItem& item)
{
    return std::string(kStorageRoot) + "/wp_" + wqn::SafePackStem(item) + ".tmp";
}

void AddManifestPackStems(
    const wqn::WqnWordPackManifest& manifest,
    std::vector<std::string>* stems)
{
    if (stems == nullptr) {
        return;
    }
    for (const wqn::WqnWordPackManifestItem& item : manifest.packs) {
        const std::string stem = wqn::SafePackStem(item);
        if (std::find(stems->begin(), stems->end(), stem) == stems->end()) {
            stems->push_back(stem);
        }
    }
}

bool VerifyFileSha256(const std::string& path, const std::string& expected)
{
    if (expected.empty()) {
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

esp_err_t ParsePackManifestFile(wqn::WqnWordPackManifest* manifest)
{
    std::string payload;
    esp_err_t result = ReadWholeFile(kManifestPath, &payload);
    if (result == ESP_OK) {
        result = wqn::ParseWordPackManifestResponse(payload, manifest);
        if (result == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(kTag, "primary word-pack manifest invalid; trying backup");
    } else if (result != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(kTag, "primary word-pack manifest unreadable; trying backup");
    }

    payload.clear();
    const esp_err_t backup_result = ReadWholeFile(kManifestBackupPath, &payload);
    if (backup_result != ESP_OK) {
        return result == ESP_ERR_NOT_FOUND ? backup_result : result;
    }
    return wqn::ParseWordPackManifestResponse(payload, manifest);
}

void PruneUnreferencedPackFiles(const wqn::WqnWordPackManifest& current)
{
    std::vector<std::string> retained_stems;
    std::vector<std::string> pinned_hash_prefixes;
    AddManifestPackStems(current, &retained_stems);

    for (const auto mode : kPersistedSessionModes) {
        wqn::PersistedWordSession pinned_session;
        if (wqn::LoadPersistedWordSession(mode, &pinned_session) == ESP_OK &&
            pinned_session.active) {
            for (const auto& snapshot : pinned_session.remote.snapshot) {
                const std::string sha256 = snapshot.sha256;
                if (sha256.size() >= kPackHashStemChars) {
                    pinned_hash_prefixes.push_back(
                        sha256.substr(0, kPackHashStemChars));
                }
            }
        }
    }

    if (FileExists(kManifestBackupPath)) {
        std::string backup_payload;
        wqn::WqnWordPackManifest backup;
        if (ReadWholeFile(kManifestBackupPath, &backup_payload) != ESP_OK ||
            wqn::ParseWordPackManifestResponse(backup_payload, &backup) != ESP_OK) {
            ESP_LOGW(kTag, "word pack backup manifest invalid; skip pruning rollback files");
            return;
        }
        AddManifestPackStems(backup, &retained_stems);
    }

    DIR* directory = opendir(kStorageRoot);
    if (directory == nullptr) {
        ESP_LOGW(kTag, "word pack prune opendir failed");
        return;
    }
    while (dirent* entry = readdir(directory)) {
        const std::string name = entry->d_name;
        constexpr char kPrefix[] = "wp_";
        constexpr char kSuffix[] = ".wqwp";
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
        const bool pinned = std::any_of(
            pinned_hash_prefixes.begin(),
            pinned_hash_prefixes.end(),
            [&](const std::string& hash) {
                return stem.size() > hash.size() &&
                    stem.compare(stem.size() - hash.size(), hash.size(), hash) == 0;
            });
        if (pinned) {
            continue;
        }
        const std::string path = std::string(kStorageRoot) + "/" + name;
        if (std::remove(path.c_str()) == 0) {
            ESP_LOGI(kTag, "pruned stale word pack: %s", name.c_str());
        } else {
            ESP_LOGW(kTag, "failed to prune stale word pack: %s", name.c_str());
        }
    }
    closedir(directory);
}

esp_err_t ResetWordPackStorageCacheRaw(void*)
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
            name.size() > 8 && name.compare(0, 3, "wp_") == 0 &&
            (name.compare(name.size() - 5, 5, ".wqwp") == 0 ||
             name.compare(name.size() - 4, 4, ".tmp") == 0);
        if (!managed_pack) {
            continue;
        }
        const std::string path = std::string(kStorageRoot) + "/" + name;
        if (std::remove(path.c_str()) != 0 && errno != ENOENT) {
            result = ESP_FAIL;
            ESP_LOGW(kTag, "failed to clear word pack cache file: %s", name.c_str());
        }
    }
    closedir(directory);
    return result;
}

esp_err_t ParsePackEntryLine(const char* line, wqn::WqnWordEntry* entry, int32_t* sort_index)
{
    if (line == nullptr || entry == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *entry = {};
    JsonDocument document(line);
    if (!document.ok() || !cJSON_IsObject(document.root())) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    entry->id = GetOptionalString(document.root(), "id");
    if (entry->id.empty()) {
        entry->id = GetOptionalString(document.root(), "word_id");
    }
    entry->deck_id = GetOptionalString(document.root(), "deck_id");
    entry->word = GetOptionalString(document.root(), "word");
    entry->normalized_word = GetOptionalString(document.root(), "normalized_word");
    entry->phonetic = GetOptionalString(document.root(), "phonetic");
    entry->meaning = GetOptionalString(document.root(), "meaning");
    entry->example = GetOptionalString(document.root(), "example");
    entry->example_translation = GetOptionalString(document.root(), "example_translation");
    entry->part_of_speech = GetOptionalString(document.root(), "part_of_speech");
    entry->status = GetOptionalString(document.root(), "status");
    entry->due_at = GetOptionalString(document.root(), "due_at");
    uint64_t parsed_revision = 0;
    int32_t parsed_sort_index = 0;
    if (entry->id.size() != 36 || entry->word.empty() || entry->word.size() > 80 ||
        entry->normalized_word.empty() || entry->normalized_word.size() > 80 ||
        !GetExactUint64(document.root(), "revision", &parsed_revision) ||
        parsed_revision == 0 || parsed_revision > std::numeric_limits<int>::max() ||
        !GetExactInt32(document.root(), "sort_index", &parsed_sort_index)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    entry->revision = static_cast<int>(parsed_revision);
    if (sort_index != nullptr) {
        *sort_index = parsed_sort_index;
    }
    return ESP_OK;
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

esp_err_t ScanPackFile(
    const wqn::WqnWordPackManifestItem& item,
    uint32_t deck_order,
    wqn::WordPackIndex* index)
{
    if (index == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const std::string path = PackPathForItem(item);
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }
    const size_t initial_entry_count = index->entries.size();
    struct EntryRollback {
        wqn::WordPackIndex* index;
        size_t initial_size;
        bool committed = false;
        ~EntryRollback()
        {
            if (!committed && index != nullptr) {
                index->entries.resize(initial_size);
            }
        }
    } rollback{index, initial_entry_count};

    // [stack-fix] 4 KB on the stack + the deep fgets->SPIFFS->esp_partition_read
    // ->tlsf_walk_pool call chain overflowed the 8 KB task stack and corrupted
    // the adjacent heap (crash in tlsf_walk_pool reading a smashed block header).
    // Keep this large buffer off the stack.
    std::vector<char> line_buffer(kLineBufferSize, 0);
    std::string line;
    esp_err_t result = ReadBoundedPackLine(file, &line_buffer, &line);
    if (result != ESP_OK || line != kPackMagic) {
        std::fclose(file);
        return result == ESP_OK ? ESP_ERR_INVALID_STATE : result;
    }

    // [mem-fix] Reserve up front so the PSRAM-backed vector doesn't realloc +
    // copy ~12 times while ingesting thousands of entries. entry_count comes
    // from the manifest; add it to the existing size (multiple packs accumulate
    // into one index) and clamp to the hard cap.
    if (item.entry_count > 0) {
        const size_t target = index->entries.size() + static_cast<size_t>(item.entry_count);
        if (target > kMaxIndexEntries) {
            std::fclose(file);
            return ESP_ERR_INVALID_SIZE;
        }
        if (target > index->entries.capacity()) {
            index->entries.reserve(target);
        }
    }

    result = ReadBoundedPackLine(file, &line_buffer, &line);
    if (result != ESP_OK) {
        std::fclose(file);
        return result;
    }
    JsonDocument metadata(line.c_str());
    uint64_t metadata_revision = 0;
    uint64_t metadata_schema = 0;
    uint64_t metadata_entries = 0;
    if (!metadata.ok() || !cJSON_IsObject(metadata.root()) ||
        GetOptionalString(metadata.root(), "deck_id") != item.deck_id ||
        !GetExactUint64(metadata.root(), "revision", &metadata_revision) ||
        metadata_revision != item.content_revision ||
        !GetExactUint64(metadata.root(), "schema_version", &metadata_schema) ||
        metadata_schema != wqn::protocol::word_study_v1::kPackSchemaVersion ||
        GetOptionalString(metadata.root(), "format") != "jsonl" ||
        GetOptionalString(metadata.root(), "compression") != "none" ||
        GetOptionalString(metadata.root(), "lexicon_type") != "english_word" ||
        !GetExactUint64(metadata.root(), "entry_count", &metadata_entries) ||
        metadata_entries != item.entry_count) {
        std::fclose(file);
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Pack stem is identical for every entry in this file; compute once.
    const std::string pack_stem = wqn::SafePackStem(item);

    uint32_t scanned_entries = 0;
    while (true) {
        const long offset = std::ftell(file);
        result = ReadBoundedPackLine(file, &line_buffer, &line);
        if (result == ESP_ERR_NOT_FOUND) {
            break;
        }
        if (result != ESP_OK || offset < 0 ||
            static_cast<unsigned long>(offset) > std::numeric_limits<uint32_t>::max()) {
            std::fclose(file);
            return result == ESP_OK ? ESP_ERR_INVALID_SIZE : result;
        }
        wqn::WqnWordEntry entry;
        int32_t sort_index = 0;
        result = ParsePackEntryLine(line.c_str(), &entry, &sort_index);
        if (result != ESP_OK) {
            std::fclose(file);
            return result;
        }

        wqn::WordPackIndexEntry indexed = {};
        CopyField(indexed.word_id, sizeof(indexed.word_id), entry.id);
        CopyField(indexed.deck_id, sizeof(indexed.deck_id), item.deck_id);
        CopyField(indexed.word, sizeof(indexed.word), entry.word);
        CopyField(indexed.normalized_word, sizeof(indexed.normalized_word), entry.normalized_word);
        CopyField(indexed.pack_stem, sizeof(indexed.pack_stem), pack_stem);
        indexed.file_offset = static_cast<uint32_t>(offset);
        indexed.deck_order = deck_order;
        indexed.sort_index = sort_index;
        index->entries.push_back(indexed);
        ++scanned_entries;
        if (index->entries.size() > kMaxIndexEntries || scanned_entries > item.entry_count) {
            std::fclose(file);
            return ESP_ERR_INVALID_SIZE;
        }
    }
    std::fclose(file);
    if (scanned_entries != item.entry_count) {
        return ESP_ERR_INVALID_SIZE;
    }
    rollback.committed = true;
    return ESP_OK;
}

esp_err_t FindPinnedPackItem(
    const wqn::StoredWordPackSnapshot& snapshot,
    wqn::WqnWordPackManifestItem* item)
{
    const std::string sha256 = snapshot.sha256;
    if (item == nullptr || sha256.size() < kPackHashStemChars) {
        return ESP_ERR_INVALID_ARG;
    }
    const std::string suffix = "_" + sha256.substr(0, kPackHashStemChars) + ".wqwp";
    DIR* directory = opendir(kStorageRoot);
    if (directory == nullptr) return ESP_FAIL;
    std::string matched_name;
    while (dirent* entry = readdir(directory)) {
        const std::string name = entry->d_name;
        if (name.size() > suffix.size() + 3 && name.compare(0, 3, "wp_") == 0 &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            matched_name = name;
            break;
        }
    }
    closedir(directory);
    if (matched_name.empty()) return ESP_ERR_NOT_FOUND;

    const std::string path = std::string(kStorageRoot) + "/" + matched_name;
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return ESP_ERR_NOT_FOUND;
    std::vector<char> line_buffer(kLineBufferSize, 0);
    std::string line;
    esp_err_t result = ReadBoundedPackLine(file, &line_buffer, &line);
    if (result != ESP_OK || line != kPackMagic) {
        std::fclose(file);
        return result == ESP_OK ? ESP_ERR_INVALID_RESPONSE : result;
    }
    result = ReadBoundedPackLine(file, &line_buffer, &line);
    std::fclose(file);
    if (result != ESP_OK) return result;

    JsonDocument metadata(line.c_str());
    uint64_t revision = 0;
    uint64_t schema_version = 0;
    uint64_t entry_count = 0;
    if (!metadata.ok() ||
        GetOptionalString(metadata.root(), "deck_id") != snapshot.deck_id ||
        !GetExactUint64(metadata.root(), "revision", &revision) ||
        revision != snapshot.content_revision ||
        !GetExactUint64(metadata.root(), "schema_version", &schema_version) ||
        schema_version != wqn::protocol::word_study_v1::kPackSchemaVersion ||
        !GetExactUint64(metadata.root(), "entry_count", &entry_count) ||
        entry_count > kMaxIndexEntries) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const std::string stem = matched_name.substr(3, matched_name.size() - 3 - 5);
    const size_t separator = stem.find('_');
    if (separator == std::string::npos || separator == 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    wqn::WqnWordPackManifestItem parsed;
    parsed.pack_id = stem.substr(0, separator);
    parsed.deck_id = snapshot.deck_id;
    parsed.revision = snapshot.pack_revision;
    parsed.pack_revision = snapshot.pack_revision;
    parsed.content_revision = snapshot.content_revision;
    parsed.schema_version = static_cast<uint32_t>(schema_version);
    parsed.format = "jsonl";
    parsed.compression = "none";
    parsed.sha256 = sha256;
    parsed.entry_count = static_cast<uint32_t>(entry_count);
    struct stat status = {};
    if (stat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode)) {
        parsed.byte_size = static_cast<uint32_t>(status.st_size);
    }
    *item = std::move(parsed);
    return ESP_OK;
}

}  // namespace

namespace wqn {

esp_err_t InitWordPackStorage()
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

esp_err_t ResetWordPackStorageCache()
{
    runtime::SleepLease storage_lease = runtime::SleepLease::TryAcquire(
        runtime::SleepBlocker::kStorage,
        "word-pack-cache-reset",
        __FILE__,
        __LINE__);
    if (!storage_lease) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t result = services::ExecuteStorageTransaction(
        ResetWordPackStorageCacheRaw, nullptr);
    if (result == ESP_OK) {
        ESP_LOGW(kTag, "cleared incompatible word pack cache; cloud content is recoverable");
    }
    return result;
}

esp_err_t LoadWordPackManifest(WqnWordPackManifest* manifest)
{
    if (manifest == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *manifest = {};
    return ParsePackManifestFile(manifest);
}

esp_err_t MergeWordPackManifestDelta(
    const WqnWordPackManifest& delta,
    WqnWordPackManifest* merged)
{
    if (merged == nullptr || delta.cursor > protocol::v3::kMaxSafeJsonInteger) {
        return ESP_ERR_INVALID_ARG;
    }
    WqnWordPackManifest current;
    const esp_err_t load_result = LoadWordPackManifest(&current);
    if (load_result != ESP_OK && load_result != ESP_ERR_NOT_FOUND) {
        return load_result;
    }
    if (load_result == ESP_ERR_NOT_FOUND) {
        current = {};
    }
    if (delta.cursor < current.cursor) {
        return ESP_ERR_INVALID_STATE;
    }

    for (const WqnWordPackManifestItem& change : delta.packs) {
        auto existing = std::find_if(
            current.packs.begin(),
            current.packs.end(),
            [&](const WqnWordPackManifestItem& item) {
                return item.deck_id == change.deck_id;
            });
        if (change.deleted) {
            if (existing != current.packs.end()) {
                current.packs.erase(existing);
            }
            continue;
        }
        if (change.deck_id.empty() || change.pack_id.empty()) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (existing == current.packs.end()) {
            current.packs.push_back(change);
        } else {
            *existing = change;
        }
    }
    std::sort(
        current.packs.begin(),
        current.packs.end(),
        [](const WqnWordPackManifestItem& left, const WqnWordPackManifestItem& right) {
            return left.deck_id < right.deck_id;
        });
    current.cursor = delta.cursor;
    current.has_more = delta.has_more;
    current.server_time = delta.server_time;
    *merged = std::move(current);
    return ESP_OK;
}

esp_err_t SaveWordPackManifestRaw(const WqnWordPackManifest& manifest)
{
    cJSON* root = cJSON_CreateObject();
    cJSON* data = cJSON_CreateObject();
    cJSON* packs = cJSON_CreateArray();
    if (root == nullptr || data == nullptr || packs == nullptr) {
        cJSON_Delete(root);
        cJSON_Delete(data);
        cJSON_Delete(packs);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddItemToObject(root, "data", data);
    cJSON_AddStringToObject(data, "server_time", manifest.server_time.c_str());
    cJSON_AddNumberToObject(data, "cursor", static_cast<double>(manifest.cursor));
    cJSON_AddBoolToObject(data, "has_more", manifest.has_more);
    cJSON_AddItemToObject(data, "packs", packs);

    for (const WqnWordPackManifestItem& item : manifest.packs) {
        cJSON* pack = cJSON_CreateObject();
        if (pack == nullptr) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(pack, "pack_id", item.pack_id.c_str());
        cJSON_AddStringToObject(pack, "deck_id", item.deck_id.c_str());
        cJSON_AddStringToObject(pack, "title", item.title.c_str());
        cJSON_AddNumberToObject(pack, "revision", static_cast<double>(item.revision));
        cJSON_AddNumberToObject(pack, "content_revision", static_cast<double>(item.content_revision));
        cJSON_AddNumberToObject(pack, "pack_revision", static_cast<double>(item.pack_revision));
        cJSON_AddNumberToObject(pack, "change_sequence", static_cast<double>(item.change_sequence));
        cJSON_AddNumberToObject(pack, "schema_version", item.schema_version);
        cJSON_AddStringToObject(pack, "format", item.format.c_str());
        cJSON_AddStringToObject(pack, "compression", item.compression.c_str());
        cJSON_AddStringToObject(pack, "sha256", item.sha256.c_str());
        cJSON_AddStringToObject(pack, "download_url", item.download_url.c_str());
        cJSON_AddNumberToObject(pack, "entry_count", item.entry_count);
        cJSON_AddNumberToObject(pack, "byte_size", item.byte_size);
        cJSON_AddItemToArray(packs, pack);
    }

    char* rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (rendered == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    FILE* file = std::fopen(kManifestTempPath, "wb");
    if (file == nullptr) {
        ESP_LOGW(kTag, "word pack manifest save fopen failed: %s", kManifestTempPath);
        cJSON_free(rendered);
        return ESP_FAIL;
    }
    const size_t length = std::strlen(rendered);
    const size_t written = std::fwrite(rendered, 1, length, file);
    const bool flushed = std::fflush(file) == 0 && ::fsync(fileno(file)) == 0;
    const bool closed = std::fclose(file) == 0;
    cJSON_free(rendered);
    if (written != length || !flushed || !closed) {
        ESP_LOGW(kTag, "word pack manifest durable write failed: want=%u got=%u",
                 static_cast<unsigned>(length), static_cast<unsigned>(written));
        std::remove(kManifestTempPath);
        return ESP_FAIL;
    }

    const bool had_primary = FileExists(kManifestPath);
    if (had_primary) {
        if (std::remove(kManifestBackupPath) != 0 && errno != ENOENT) {
            ESP_LOGW(kTag, "word pack manifest stale backup remove failed");
            std::remove(kManifestTempPath);
            return ESP_FAIL;
        }
        if (std::rename(kManifestPath, kManifestBackupPath) != 0) {
            ESP_LOGW(kTag, "word pack manifest backup commit failed");
            std::remove(kManifestTempPath);
            return ESP_FAIL;
        }
    }
    if (std::rename(kManifestTempPath, kManifestPath) != 0) {
        ESP_LOGW(kTag, "word pack manifest commit rename failed");
        if (had_primary && std::rename(kManifestBackupPath, kManifestPath) != 0) {
            ESP_LOGE(kTag, "word pack manifest rollback rename failed; backup remains readable");
        }
        std::remove(kManifestTempPath);
        return ESP_FAIL;
    }
    PruneUnreferencedPackFiles(manifest);
    return ESP_OK;
}

esp_err_t SaveWordPackManifestTransaction(void* opaque)
{
    return SaveWordPackManifestRaw(
        *static_cast<const WqnWordPackManifest*>(opaque));
}

esp_err_t SaveWordPackManifest(const WqnWordPackManifest& manifest)
{
    runtime::SleepLease storage_lease = runtime::SleepLease::TryAcquire(
        runtime::SleepBlocker::kStorage, "word-pack-manifest", __FILE__, __LINE__);
    if (!storage_lease) {
        return ESP_ERR_INVALID_STATE;
    }
    return services::ExecuteStorageTransaction(
        SaveWordPackManifestTransaction,
        const_cast<WqnWordPackManifest*>(&manifest));
}

esp_err_t LoadWordPackIndexInternal(
    const PersistedWordSession* pinned_session,
    WordPackIndex* index)
{
    if (index == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    // SHA verification, JSONL scanning and dictionary sorting are CPU-bound.
    // Keep maximum frequency scoped to this rebuild instead of disabling DFS.
    auto cpu_lease = runtime::CpuPerformanceLease::TryAcquire();
    *index = WordPackIndex{};
    index->mounted = InitWordPackStorage() == ESP_OK;
    if (!index->mounted) {
        index->status_message = "词库分区不可用";
        return ESP_OK;
    }

    WqnWordPackManifest manifest;
    esp_err_t manifest_result = ParsePackManifestFile(&manifest);
    if (manifest_result == ESP_ERR_NOT_FOUND) {
        index->status_message = "词库未同步";
        return ESP_OK;
    }
    if (manifest_result != ESP_OK) {
        index->pack_error = true;
        index->status_message = "词库清单损坏";
        return ESP_OK;
    }

    bool pinned_pack_missing = false;
    if (pinned_session != nullptr && pinned_session->active) {
        for (const auto& snapshot : pinned_session->remote.snapshot) {
            auto existing = std::find_if(
                manifest.packs.begin(),
                manifest.packs.end(),
                [&](const WqnWordPackManifestItem& value) {
                    return value.deck_id == snapshot.deck_id;
                });
            if (existing != manifest.packs.end() &&
                existing->sha256 == snapshot.sha256) {
                continue;
            }
            WqnWordPackManifestItem pinned;
            if (FindPinnedPackItem(snapshot, &pinned) == ESP_OK) {
                if (existing == manifest.packs.end()) {
                    manifest.packs.push_back(std::move(pinned));
                } else {
                    *existing = std::move(pinned);
                }
            } else {
                pinned_pack_missing = true;
                if (existing != manifest.packs.end()) {
                    manifest.packs.erase(existing);
                }
            }
        }
    }
    index->has_manifest = true;
    index->pack_count = manifest.packs.size();
    index->pack_identities.reserve(manifest.packs.size());
    for (const WqnWordPackManifestItem& item : manifest.packs) {
        if (item.deck_id.size() != 36 || item.sha256.size() != 64) {
            index->pack_error = true;
            index->status_message = "词库清单无效";
            return ESP_OK;
        }
        WordPackIdentity identity;
        std::snprintf(
            identity.deck_id,
            sizeof(identity.deck_id),
            "%s",
            item.deck_id.c_str());
        identity.content_revision = item.content_revision;
        identity.pack_revision = item.pack_revision;
        std::snprintf(
            identity.sha256,
            sizeof(identity.sha256),
            "%s",
            item.sha256.c_str());
        index->pack_identities.push_back(identity);
    }

    size_t expected_entries = 0;
    for (const WqnWordPackManifestItem& item : manifest.packs) {
        if (item.entry_count > kMaxIndexEntries - expected_entries) {
            index->pack_error = true;
            index->status_message = "词库条目过多";
            return ESP_OK;
        }
        expected_entries += item.entry_count;
    }
    const size_t required_psram =
        expected_entries * sizeof(WordPackIndexEntry) +
        expected_entries * sizeof(uint32_t);
    constexpr size_t kIndexAllocationReserveBytes = 64U * 1024U;
    const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (required_psram > free_psram ||
        free_psram - required_psram < kIndexAllocationReserveBytes) {
        index->pack_error = true;
        index->status_message = "词库索引内存不足";
        ESP_LOGW(
            kTag,
            "word pack index PSRAM preflight failed: need=%u free=%u reserve=%u",
            static_cast<unsigned>(required_psram),
            static_cast<unsigned>(free_psram),
            static_cast<unsigned>(kIndexAllocationReserveBytes));
        return ESP_OK;
    }
    index->entries.reserve(expected_entries);
    index->dictionary_order.reserve(expected_entries);

    for (const WqnWordPackManifestItem& item : manifest.packs) {
        const std::string path = PackPathForItem(item);
        struct stat st = {};
        if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            index->pack_bytes += static_cast<size_t>(st.st_size);
        }
        if (!VerifyFileSha256(path, item.sha256)) {
            index->pack_error = true;
            index->status_message = "词库校验失败";
            continue;
        }
        index->manifest_revision = std::max(index->manifest_revision, item.change_sequence);
        const esp_err_t scan_result = ScanPackFile(
            item,
            static_cast<uint32_t>(&item - manifest.packs.data()),
            index);
        if (scan_result != ESP_OK) {
            index->pack_error = true;
            index->status_message = "词库读取失败";
        }
    }

    // Preserve entries in stable deck/import order for sequential study. The
    // dictionary gets its own compact indirection array so lookup never
    // rewrites the study order.
    for (size_t entry_index = 0; entry_index < index->entries.size(); ++entry_index) {
        index->dictionary_order.push_back(static_cast<uint32_t>(entry_index));
    }
    std::sort(
        index->dictionary_order.begin(),
        index->dictionary_order.end(),
        [&](uint32_t left, uint32_t right) {
            const WordPackIndexEntry& a = index->entries[left];
            const WordPackIndexEntry& b = index->entries[right];
            const int word_compare = std::strcmp(a.normalized_word, b.normalized_word);
            return word_compare != 0 ? word_compare < 0 : std::strcmp(a.word_id, b.word_id) < 0;
        });

    if (index->entries.empty() && index->status_message.empty()) {
        index->status_message = "词库为空";
    } else if (pinned_pack_missing) {
        index->pack_error = true;
        index->status_message = "会话词包缺失";
    } else if (index->status_message.empty()) {
        index->status_message = "词库已就绪";
    }

    ESP_LOGI(
        kTag,
        "word pack index: pack_count=%u word_index_count=%u pack_bytes=%u free_internal=%u free_psram=%u truncated=%d",
        static_cast<unsigned>(index->pack_count),
        static_cast<unsigned>(index->entries.size()),
        static_cast<unsigned>(index->pack_bytes),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
        index->truncated ? 1 : 0);
    return ESP_OK;
}

esp_err_t LoadWordPackIndex(WordPackIndex* index)
{
    for (const auto mode : kPersistedSessionModes) {
        PersistedWordSession session;
        if (LoadPersistedWordSession(mode, &session) == ESP_OK &&
            session.active && !session.paused) {
            return LoadWordPackIndexInternal(&session, index);
        }
    }
    return LoadWordPackIndexInternal(nullptr, index);
}

esp_err_t LoadWordPackIndexForSession(
    const PersistedWordSession& session,
    WordPackIndex* index)
{
    return LoadWordPackIndexInternal(&session, index);
}

bool WordPackIndexMatchesSession(
    const WordPackIndex& index,
    const PersistedWordSession& session)
{
    if (!index.mounted || !index.has_manifest || index.pack_error ||
        !session.active || session.remote.snapshot.empty()) {
        return false;
    }
    for (const StoredWordPackSnapshot& snapshot : session.remote.snapshot) {
        const auto identity = std::find_if(
            index.pack_identities.begin(),
            index.pack_identities.end(),
            [&](const WordPackIdentity& value) {
                return std::strcmp(value.deck_id, snapshot.deck_id) == 0;
            });
        if (identity == index.pack_identities.end() ||
            identity->content_revision != snapshot.content_revision ||
            identity->pack_revision != snapshot.pack_revision ||
            std::strcmp(identity->sha256, snapshot.sha256) != 0) {
            return false;
        }
    }
    return true;
}

enum class WordPackStreamOperation : uint8_t {
    kBegin,
    kAppend,
    kCommit,
    kAbort,
};

struct WordPackStreamContext {
    const WqnWordPackManifestItem* item = nullptr;
    FILE* file = nullptr;
    mbedtls_sha256_context sha = {};
    bool sha_started = false;
    size_t bytes_written = 0;
    const uint8_t* chunk = nullptr;
    size_t chunk_size = 0;
    WordPackStreamOperation operation = WordPackStreamOperation::kBegin;
};

esp_err_t WordPackStreamTransaction(void* opaque)
{
    auto* context = static_cast<WordPackStreamContext*>(opaque);
    if (context == nullptr || context->item == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const std::string temp_path = TempPackPathForItem(*context->item);
    const std::string final_path = PackPathForItem(*context->item);
    switch (context->operation) {
        case WordPackStreamOperation::kBegin:
            std::remove(temp_path.c_str());
            context->file = std::fopen(temp_path.c_str(), "wb");
            if (context->file == nullptr) {
                ESP_LOGW(
                    kTag,
                    "word pack temp open failed: path=%s errno=%d",
                    temp_path.c_str(),
                    errno);
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

        case WordPackStreamOperation::kAppend:
            if (context->file == nullptr || !context->sha_started ||
                context->chunk == nullptr || context->chunk_size == 0 ||
                context->bytes_written + context->chunk_size > context->item->byte_size ||
                context->bytes_written + context->chunk_size > kMaxPackBytes) {
                return ESP_ERR_INVALID_SIZE;
            }
            if (std::fwrite(
                    context->chunk, 1, context->chunk_size, context->file) !=
                context->chunk_size) {
                return ESP_FAIL;
            }
            if (mbedtls_sha256_update(
                    &context->sha, context->chunk, context->chunk_size) != 0) {
                return ESP_FAIL;
            }
            context->bytes_written += context->chunk_size;
            return ESP_OK;

        case WordPackStreamOperation::kCommit: {
            if (context->file == nullptr || !context->sha_started ||
                context->bytes_written != context->item->byte_size) {
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
            if (actual_sha != context->item->sha256) {
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

        case WordPackStreamOperation::kAbort:
            if (context->file != nullptr) {
                std::fclose(context->file);
                context->file = nullptr;
            }
            std::remove(temp_path.c_str());
            return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t AppendWordPackStream(
    void* opaque,
    const uint8_t* bytes,
    size_t size)
{
    auto* context = static_cast<WordPackStreamContext*>(opaque);
    if (context == nullptr || bytes == nullptr || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    context->operation = WordPackStreamOperation::kAppend;
    context->chunk = bytes;
    context->chunk_size = size;
    return services::ExecuteStorageTransaction(
        WordPackStreamTransaction, context);
}

esp_err_t DownloadWordPackToStorage(
    const std::string& token,
    const protocol::v3::RequestMetadata& metadata,
    const WqnWordPackManifestItem& item)
{
    if (item.pack_id.empty() || item.sha256.size() != 64 ||
        item.schema_version != protocol::word_study_v1::kPackSchemaVersion ||
        item.entry_count > kMaxIndexEntries || item.byte_size == 0 ||
        item.byte_size > kMaxPackBytes) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!WordPackNeedsDownload(item)) {
        return ESP_OK;
    }
    runtime::SleepLease storage_lease = runtime::SleepLease::TryAcquire(
        runtime::SleepBlocker::kStorage, "word-pack-stream", __FILE__, __LINE__);
    if (!storage_lease) {
        return ESP_ERR_INVALID_STATE;
    }
    WordPackStreamContext context;
    context.item = &item;
    mbedtls_sha256_init(&context.sha);
    context.operation = WordPackStreamOperation::kBegin;
    esp_err_t result = services::ExecuteStorageTransaction(
        WordPackStreamTransaction, &context);
    if (result == ESP_OK) {
        result = DownloadWordPackStream(
            token, metadata, item, AppendWordPackStream, &context);
    }
    if (result == ESP_OK) {
        context.operation = WordPackStreamOperation::kCommit;
        result = services::ExecuteStorageTransaction(
            WordPackStreamTransaction, &context);
    }
    if (result != ESP_OK) {
        context.operation = WordPackStreamOperation::kAbort;
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            services::ExecuteStorageTransaction(
                WordPackStreamTransaction, &context));
    }
    mbedtls_sha256_free(&context.sha);
    return result;
}

bool WordPackNeedsDownload(const WqnWordPackManifestItem& item)
{
    const std::string path = PackPathForItem(item);
    return !FileExists(path) || !VerifyFileSha256(path, item.sha256);
}

esp_err_t ReadWordPackEntry(const WordPackIndexEntry& index_entry, WqnWordEntry* entry)
{
    if (entry == nullptr || index_entry.pack_stem[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    // A card turn performs a bounded SPIFFS seek, JSONL read and cJSON parse.
    // Keep that foreground work off the 40 MHz DFS floor as well.
    auto cpu_lease = runtime::CpuPerformanceLease::TryAcquire();
    *entry = WqnWordEntry{};

    const std::string path = PackPathForStem(index_entry.pack_stem);
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }
    if (std::fseek(file, static_cast<long>(index_entry.file_offset), SEEK_SET) != 0) {
        std::fclose(file);
        return ESP_FAIL;
    }
    // [stack-fix] 4 KB on the stack + the deep fgets->SPIFFS->esp_partition_read
    // ->tlsf_walk_pool call chain overflowed the 8 KB task stack and corrupted
    // the adjacent heap (crash in tlsf_walk_pool reading a smashed block header).
    // Keep this large buffer off the stack.
    std::vector<char> line_buffer(kLineBufferSize, 0);
    std::string line;
    const esp_err_t line_result = ReadBoundedPackLine(file, &line_buffer, &line);
    std::fclose(file);
    if (line_result != ESP_OK) {
        return line_result;
    }

    const esp_err_t parse_result = ParsePackEntryLine(line.c_str(), entry, nullptr);
    if (parse_result != ESP_OK) {
        return parse_result;
    }
    entry->id = index_entry.word_id;
    entry->deck_id = index_entry.deck_id;
    return ESP_OK;
}

void FindWordPackPrefixMatches(const WordPackIndex& index, const std::string& prefix, size_t limit, std::vector<size_t>* matches)
{
    if (matches == nullptr) {
        return;
    }
    matches->clear();
    const std::string normalized = NormalizeWordLookupText(prefix);
    for (const uint32_t entry_index : index.dictionary_order) {
        if (entry_index >= index.entries.size()) {
            continue;
        }
        const WordPackIndexEntry& entry = index.entries[entry_index];
        if (!normalized.empty()) {
            const int cmp = std::strncmp(
                entry.normalized_word,
                normalized.c_str(),
                normalized.size());
            if (cmp < 0) {
                continue;
            }
            if (cmp > 0) {
                break;
            }
        }
        matches->push_back(static_cast<size_t>(entry_index));
        if (matches->size() >= limit) {
            break;
        }
    }
}

std::vector<char> WordPackNextLetters(const WordPackIndex& index, const std::string& prefix)
{
    const std::string normalized = NormalizeWordLookupText(prefix);
    std::vector<char> letters;
    for (const uint32_t entry_index : index.dictionary_order) {
        if (entry_index >= index.entries.size()) {
            continue;
        }
        const WordPackIndexEntry& entry = index.entries[entry_index];
        const size_t nw_len = std::strlen(entry.normalized_word);
        if (nw_len <= normalized.size()) {
            // Entry is shorter than prefix; if sorted, skip (don't break
            // - a shorter entry could still be "before" the prefix).
            continue;
        }
        if (!normalized.empty()) {
            const int cmp = std::strncmp(entry.normalized_word, normalized.c_str(), normalized.size());
            if (cmp < 0) {
                continue;  // before prefix range
            }
            if (cmp > 0) {
                break;  // past prefix range, done
            }
        }
        const char next = entry.normalized_word[normalized.size()];
        if (next < 'a' || next > 'z') {
            continue;
        }
        if (std::find(letters.begin(), letters.end(), next) == letters.end()) {
            letters.push_back(next);
        }
    }
    std::sort(letters.begin(), letters.end());
    return letters;
}

std::string NormalizeWordLookupText(const std::string& value)
{
    std::string normalized;
    normalized.reserve(value.size());
    for (const unsigned char ch : value) {
        if (ch >= 'A' && ch <= 'Z') {
            normalized.push_back(static_cast<char>(ch - 'A' + 'a'));
        } else if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '\'') {
            normalized.push_back(static_cast<char>(ch));
        }
    }
    return normalized;
}

}  // namespace wqn
