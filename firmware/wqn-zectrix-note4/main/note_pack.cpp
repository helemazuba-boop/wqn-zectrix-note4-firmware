#include "note_pack.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <utime.h>
#include <unistd.h>
#include <utility>

#include "cJSON.h"
#include "device_protocol/note_study.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"
#include "runtime/sleep_coordinator.h"
#include "services/storage_service.h"

namespace {

constexpr char kTag[] = "note_pack";
constexpr char kStorageRoot[] = "/storage";
constexpr char kManifestPath[] = "/storage/np_manifest.json";
constexpr char kManifestTempPath[] = "/storage/np_manifest.tmp";
constexpr char kManifestBackupPath[] = "/storage/np_manifest.bak";
constexpr uint64_t kPackSchemaVersion =
    wqn::protocol::note_study_v1::kPackSchemaVersion;
// Per-pack cap comes from the contract; the index sums packs so it needs its own
// global cap. A single body read is O(1), so the ceiling is really PSRAM, which
// the LoadNotePackIndex preflight enforces exactly.
constexpr size_t kMaxPackEntries = wqn::protocol::note_study_v1::kMaxPackEntries;
constexpr size_t kMaxIndexEntries = 8000;
constexpr size_t kMaxPackBytes = wqn::protocol::note_study_v1::kMaxPackBytes;
constexpr size_t kMaxLineBytes = wqn::protocol::note_study_v1::kMaxPackLineBytes;
// A device browses a bounded set of notebooks; cap the persisted aggregate so a
// runaway account cannot grow the manifest without limit.
constexpr size_t kMaxStoredNotebooks = 200;
// plain_text_v1 limits (see WQN/web/lib/note-content-format.ts): 120-char title,
// 4000-char / 16 KiB body. Titles are bounded in bytes at 4 bytes/char.
constexpr size_t kMaxNoteTitleBytes = 4 * 120;
constexpr size_t kMaxNoteContentBytes = 16384;
constexpr size_t kPackIdStemChars = 6;
constexpr size_t kPackHashStemChars = 12;
// SPIFFS counts the leading slash in its object name and reserves one byte for
// NUL. Keep the longest final suffix (.wqnp) strictly within that budget.
constexpr size_t kMaxPackObjectNameBytes =
    1 + 3 + kPackIdStemChars + 1 + kPackHashStemChars + 5;
static_assert(
    kMaxPackObjectNameBytes <= CONFIG_SPIFFS_OBJ_NAME_LEN - 1,
    "note pack filename exceeds SPIFFS object-name budget");
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

void ReleaseNotePackIndexAllocations(wqn::NotePackIndex* index)
{
    if (index == nullptr) return;
    // Swap with empty vectors so the PSRAM allocator releases capacity even when
    // index construction exits with pack_error; clearing alone keeps capacity
    // attached and makes each retry start with less memory than the last.
    std::vector<
        wqn::NotePackIndexEntry,
        wqn::NotePsramAllocator<wqn::NotePackIndexEntry>> empty_entries;
    index->entries.swap(empty_entries);
    std::vector<uint32_t, wqn::NotePsramAllocator<uint32_t>> empty_order;
    index->note_order.swap(empty_order);
    std::vector<
        wqn::NotePackIdentity,
        wqn::NotePsramAllocator<wqn::NotePackIdentity>> empty_identities;
    index->pack_identities.swap(empty_identities);
}

// Reads one JSONL line. Unlike the word pack, a note pack body is
// `lines.join('\n')` with no magic header and no trailing newline, so the final
// record is terminated by EOF rather than LF. That EOF-terminated line is valid;
// a missing LF that is NOT at EOF means the line overflowed the bounded buffer.
esp_err_t ReadBoundedNotePackLine(FILE* file, std::vector<char>* buffer, std::string* line)
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

// Copies a display title, truncating on a UTF-8 boundary so the list never shows
// a split multi-byte character. The full title stays available via a body read.
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

}  // namespace

namespace wqn {

std::string SafeNotePackStem(const WqnNotePackManifestNotebook& notebook)
{
    std::string stem;
    stem.reserve(kPackIdStemChars + 1 + kPackHashStemChars);
    for (const char ch : notebook.pack_id) {
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
        stem = "note";
    }
    if (!notebook.sha256.empty()) {
        stem.push_back('_');
        size_t hash_chars = 0;
        for (const char ch : notebook.sha256) {
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
    return std::string(kStorageRoot) + "/np_" + stem + ".wqnp";
}

std::string PackPathForNotebook(const wqn::WqnNotePackManifestNotebook& notebook)
{
    return PackPathForStem(wqn::SafeNotePackStem(notebook));
}

std::string TempPackPathForNotebook(const wqn::WqnNotePackManifestNotebook& notebook)
{
    return std::string(kStorageRoot) + "/np_" + wqn::SafeNotePackStem(notebook) + ".tmp";
}

void AddManifestNotebookStems(
    const wqn::WqnNotePackManifest& manifest,
    std::vector<std::string>* stems)
{
    if (stems == nullptr) {
        return;
    }
    for (const wqn::WqnNotePackManifestNotebook& notebook : manifest.notebooks) {
        if (!notebook.has_pack) {
            continue;
        }
        const std::string stem = wqn::SafeNotePackStem(notebook);
        if (std::find(stems->begin(), stems->end(), stem) == stems->end()) {
            stems->push_back(stem);
        }
    }
}

esp_err_t ParseNoteManifestPayload(const std::string& payload, wqn::WqnNotePackManifest* manifest)
{
    *manifest = {};
    JsonDocument document(payload.c_str());
    cJSON* data = document.ok()
        ? cJSON_GetObjectItemCaseSensitive(document.root(), "data")
        : nullptr;
    cJSON* has_more = data != nullptr
        ? cJSON_GetObjectItemCaseSensitive(data, "has_more")
        : nullptr;
    cJSON* notebooks = data != nullptr
        ? cJSON_GetObjectItemCaseSensitive(data, "notebooks")
        : nullptr;
    if (!document.ok() || !cJSON_IsObject(data) ||
        !GetExactUint64(data, "cursor", &manifest->cursor) ||
        !cJSON_IsBool(has_more) || !cJSON_IsArray(notebooks)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    manifest->has_more = cJSON_IsTrue(has_more);

    const int count = cJSON_GetArraySize(notebooks);
    if (count < 0 || static_cast<size_t>(count) > kMaxStoredNotebooks) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    manifest->notebooks.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
        cJSON* object = cJSON_GetArrayItem(notebooks, index);
        wqn::WqnNotePackManifestNotebook notebook;
        notebook.notebook_id = GetOptionalString(object, "notebook_id");
        notebook.title = GetOptionalString(object, "title");
        cJSON* has_pack = cJSON_GetObjectItemCaseSensitive(object, "has_pack");
        if (!cJSON_IsObject(object) || notebook.notebook_id.size() != 36 ||
            notebook.title.empty() || notebook.title.size() > 240 ||
            !cJSON_IsBool(has_pack) ||
            !GetExactUint64(object, "change_sequence", &notebook.change_sequence) ||
            !GetExactUint64(object, "content_revision", &notebook.content_revision)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        notebook.has_pack = cJSON_IsTrue(has_pack);
        cJSON* pack = cJSON_GetObjectItemCaseSensitive(object, "pack");
        if (notebook.has_pack) {
            uint64_t schema_version = 0;
            uint64_t entry_count = 0;
            uint64_t byte_size = 0;
            notebook.pack_id = GetOptionalString(pack, "pack_id");
            notebook.sha256 = GetOptionalString(pack, "sha256");
            notebook.download_url = GetOptionalString(pack, "download_url");
            if (!cJSON_IsObject(pack) || notebook.pack_id.size() != 36 ||
                notebook.sha256.size() != 64 ||
                !GetExactUint64(pack, "pack_revision", &notebook.pack_revision) ||
                !GetExactUint64(pack, "schema_version", &schema_version) ||
                !GetExactUint64(pack, "entry_count", &entry_count) ||
                !GetExactUint64(pack, "byte_size", &byte_size) ||
                schema_version != kPackSchemaVersion ||
                entry_count > kMaxPackEntries || byte_size == 0 ||
                byte_size > kMaxPackBytes) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            notebook.schema_version = static_cast<uint32_t>(schema_version);
            notebook.entry_count = static_cast<uint32_t>(entry_count);
            notebook.byte_size = static_cast<uint32_t>(byte_size);
        }
        manifest->notebooks.push_back(std::move(notebook));
    }
    return ESP_OK;
}

// Loads the persisted note-pack manifest, falling back to the .bak copy if the
// primary file is missing or corrupt (mirrors the atomic save's rollback).
esp_err_t ParseStoredNoteManifest(wqn::WqnNotePackManifest* manifest)
{
    if (manifest == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *manifest = {};
    std::string payload;
    esp_err_t result = ReadWholeFile(kManifestPath, &payload);
    if (result == ESP_OK) {
        result = ParseNoteManifestPayload(payload, manifest);
        if (result == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(kTag, "primary note pack manifest invalid; trying backup");
    } else if (result != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(kTag, "primary note pack manifest unreadable; trying backup");
    }

    payload.clear();
    const esp_err_t backup_result = ReadWholeFile(kManifestBackupPath, &payload);
    if (backup_result != ESP_OK) {
        return result == ESP_ERR_NOT_FOUND ? backup_result : result;
    }
    return ParseNoteManifestPayload(payload, manifest);
}

void PruneUnreferencedNotePackFiles(const wqn::WqnNotePackManifest& current)
{
    std::vector<std::string> retained_stems;
    AddManifestNotebookStems(current, &retained_stems);

    if (FileExists(kManifestBackupPath)) {
        wqn::WqnNotePackManifest backup;
        std::string backup_payload;
        // Best-effort: keep files referenced by a readable backup so a rollback
        // still has its packs. A corrupt backup simply prunes more aggressively.
        if (ReadWholeFile(kManifestBackupPath, &backup_payload) == ESP_OK) {
            JsonDocument document(backup_payload.c_str());
            cJSON* data = document.ok()
                ? cJSON_GetObjectItemCaseSensitive(document.root(), "data")
                : nullptr;
            cJSON* notebooks = data != nullptr
                ? cJSON_GetObjectItemCaseSensitive(data, "notebooks")
                : nullptr;
            const int count = cJSON_GetArraySize(notebooks);
            for (int index = 0; index < count; ++index) {
                cJSON* object = cJSON_GetArrayItem(notebooks, index);
                cJSON* has_pack = cJSON_GetObjectItemCaseSensitive(object, "has_pack");
                if (!cJSON_IsTrue(has_pack)) {
                    continue;
                }
                wqn::WqnNotePackManifestNotebook notebook;
                cJSON* pack = cJSON_GetObjectItemCaseSensitive(object, "pack");
                notebook.pack_id = GetOptionalString(pack, "pack_id");
                notebook.sha256 = GetOptionalString(pack, "sha256");
                if (notebook.pack_id.empty() || notebook.sha256.empty()) {
                    continue;
                }
                const std::string stem = wqn::SafeNotePackStem(notebook);
                if (std::find(retained_stems.begin(), retained_stems.end(), stem) ==
                    retained_stems.end()) {
                    retained_stems.push_back(stem);
                }
            }
        }
    }

    DIR* directory = opendir(kStorageRoot);
    if (directory == nullptr) {
        ESP_LOGW(kTag, "note pack prune opendir failed");
        return;
    }
    while (dirent* entry = readdir(directory)) {
        const std::string name = entry->d_name;
        constexpr char kPrefix[] = "np_";
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
            ESP_LOGI(kTag, "pruned stale note pack: %s", name.c_str());
        } else {
            ESP_LOGW(kTag, "failed to prune stale note pack: %s", name.c_str());
        }
    }
    closedir(directory);
}

esp_err_t ResetNotePackStorageCacheRaw(void*)
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
            name.size() > 8 && name.compare(0, 3, "np_") == 0 &&
            (name.compare(name.size() - 5, 5, ".wqnp") == 0 ||
             name.compare(name.size() - 4, 4, ".tmp") == 0);
        if (!managed_pack) {
            continue;
        }
        const std::string path = std::string(kStorageRoot) + "/" + name;
        if (std::remove(path.c_str()) != 0 && errno != ENOENT) {
            result = ESP_FAIL;
            ESP_LOGW(kTag, "failed to clear note pack cache file: %s", name.c_str());
        }
    }
    closedir(directory);
    return result;
}

esp_err_t ParseNoteRecordLine(const char* line, wqn::WqnNoteEntry* entry, bool include_content)
{
    if (line == nullptr || entry == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *entry = {};
    JsonDocument document(line);
    if (!document.ok() || !cJSON_IsObject(document.root())) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    entry->note_id = GetOptionalString(document.root(), "note_id");
    entry->notebook_id = GetOptionalString(document.root(), "notebook_id");
    entry->title = GetOptionalString(document.root(), "title");
    // Index scans only need metadata + offset, so skip materialising the note
    // body (up to 16 KB). Copying it into a std::string for every record just to
    // discard it churned the heap and stretched index builds to seconds; the
    // body is parsed on demand when a note is actually opened.
    if (include_content) {
        entry->content = GetOptionalString(document.root(), "content");
    }
    uint64_t revision = 0;
    int32_t sort_index = 0;
    if (entry->note_id.size() != 36 || entry->notebook_id.size() != 36 ||
        entry->title.empty() || entry->title.size() > kMaxNoteTitleBytes ||
        (include_content &&
         (entry->content.empty() || entry->content.size() > kMaxNoteContentBytes)) ||
        !GetExactInt32(document.root(), "sort_index", &sort_index) ||
        !GetExactUint64(document.root(), "revision", &revision) ||
        revision == 0 || revision > std::numeric_limits<int>::max()) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    entry->sort_index = sort_index;
    entry->revision = static_cast<int>(revision);
    // Optional e-ink image attachments. Absent on packs built before the image
    // feature; when present it must be a well-formed array of <= 4 SHA-256 ids.
    cJSON* image_ids = cJSON_GetObjectItemCaseSensitive(document.root(), "image_ids");
    if (image_ids != nullptr && !cJSON_IsNull(image_ids)) {
        if (!cJSON_IsArray(image_ids) || cJSON_GetArraySize(image_ids) > 4) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        cJSON* image_id = nullptr;
        cJSON_ArrayForEach(image_id, image_ids) {
            if (!cJSON_IsString(image_id) || image_id->valuestring == nullptr ||
                std::strlen(image_id->valuestring) != 64) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            entry->image_ids.emplace_back(image_id->valuestring);
        }
    }
    cJSON* gray4_ids =
        cJSON_GetObjectItemCaseSensitive(document.root(), "gray4_image_ids");
    if (gray4_ids == nullptr || cJSON_IsNull(gray4_ids)) {
        entry->gray4_image_ids.resize(entry->image_ids.size());
    } else {
        if (!cJSON_IsArray(gray4_ids) ||
            cJSON_GetArraySize(gray4_ids) !=
                static_cast<int>(entry->image_ids.size())) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        cJSON* gray4_id = nullptr;
        cJSON_ArrayForEach(gray4_id, gray4_ids) {
            if (cJSON_IsNull(gray4_id)) {
                entry->gray4_image_ids.emplace_back();
            } else if (cJSON_IsString(gray4_id) &&
                       gray4_id->valuestring != nullptr &&
                       std::strlen(gray4_id->valuestring) == 64) {
                entry->gray4_image_ids.emplace_back(gray4_id->valuestring);
            } else {
                return ESP_ERR_INVALID_RESPONSE;
            }
        }
    }
    return ESP_OK;
}

esp_err_t ScanNotePackFile(
    const wqn::WqnNotePackManifestNotebook& notebook,
    uint32_t notebook_order,
    wqn::NotePackIndex* index)
{
    if (index == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const std::string path = PackPathForNotebook(notebook);
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }
    const size_t initial_entry_count = index->entries.size();
    struct EntryRollback {
        wqn::NotePackIndex* index;
        size_t initial_size;
        bool committed = false;
        ~EntryRollback()
        {
            if (!committed && index != nullptr) {
                index->entries.resize(initial_size);
            }
        }
    } rollback{index, initial_entry_count};

    // [stack-fix] Keep the ~64 KB line buffer off the 8 KB task stack; the deep
    // fgets->SPIFFS->esp_partition_read chain otherwise smashes the heap.
    std::vector<char> line_buffer(kLineBufferSize, 0);
    std::string line;
    // Line 1 is the metadata record (there is no magic header).
    esp_err_t result = ReadBoundedNotePackLine(file, &line_buffer, &line);
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
        GetOptionalString(metadata.root(), "notebook_id") != notebook.notebook_id ||
        !GetExactUint64(metadata.root(), "content_revision", &metadata_revision) ||
        metadata_revision != notebook.content_revision ||
        !GetExactUint64(metadata.root(), "count", &metadata_count) ||
        metadata_count != notebook.entry_count) {
        std::fclose(file);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (notebook.entry_count > 0) {
        const size_t target = index->entries.size() + static_cast<size_t>(notebook.entry_count);
        if (target > kMaxIndexEntries) {
            std::fclose(file);
            return ESP_ERR_INVALID_SIZE;
        }
        if (target > index->entries.capacity()) {
            index->entries.reserve(target);
        }
    }

    const std::string pack_stem = wqn::SafeNotePackStem(notebook);
    uint32_t scanned_entries = 0;
    while (true) {
        const long offset = std::ftell(file);
        result = ReadBoundedNotePackLine(file, &line_buffer, &line);
        if (result == ESP_ERR_NOT_FOUND) {
            break;
        }
        if (result != ESP_OK || offset < 0 ||
            static_cast<unsigned long>(offset) > std::numeric_limits<uint32_t>::max()) {
            std::fclose(file);
            return result == ESP_OK ? ESP_ERR_INVALID_SIZE : result;
        }
        wqn::WqnNoteEntry entry;
        result = ParseNoteRecordLine(line.c_str(), &entry, /*include_content=*/false);
        if (result != ESP_OK) {
            std::fclose(file);
            return result;
        }
        if (entry.notebook_id != notebook.notebook_id) {
            std::fclose(file);
            return ESP_ERR_INVALID_RESPONSE;
        }
        wqn::NotePackIndexEntry indexed = {};
        CopyField(indexed.note_id, sizeof(indexed.note_id), entry.note_id);
        CopyField(indexed.notebook_id, sizeof(indexed.notebook_id), notebook.notebook_id);
        CopyField(indexed.pack_stem, sizeof(indexed.pack_stem), pack_stem);
        CopyTitleUtf8Safe(indexed.title, sizeof(indexed.title), entry.title);
        indexed.file_offset = static_cast<uint32_t>(offset);
        indexed.notebook_order = notebook_order;
        indexed.sort_index = entry.sort_index;
        indexed.image_count = static_cast<uint8_t>(entry.image_ids.size());
        index->entries.push_back(indexed);
        ++scanned_entries;
        if (index->entries.size() > kMaxIndexEntries || scanned_entries > notebook.entry_count) {
            std::fclose(file);
            return ESP_ERR_INVALID_SIZE;
        }
    }
    std::fclose(file);
    if (scanned_entries != notebook.entry_count) {
        return ESP_ERR_INVALID_SIZE;
    }
    rollback.committed = true;
    return ESP_OK;
}

}  // namespace

namespace wqn {

esp_err_t InitNotePackStorage()
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

esp_err_t ResetNotePackStorageCache()
{
    runtime::SleepLease storage_lease = runtime::SleepLease::TryAcquire(
        runtime::SleepBlocker::kStorage,
        "note-pack-cache-reset",
        __FILE__,
        __LINE__);
    if (!storage_lease) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t result = services::ExecuteStorageTransaction(
        ResetNotePackStorageCacheRaw, nullptr);
    if (result == ESP_OK) {
        ESP_LOGW(kTag, "cleared incompatible note pack cache; cloud content is recoverable");
    }
    return result;
}

esp_err_t LoadNotePackManifest(WqnNotePackManifest* manifest)
{
    if (manifest == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *manifest = {};
    return ParseStoredNoteManifest(manifest);
}

esp_err_t MergeNotePackManifestDelta(
    const WqnNotePackManifest& delta,
    WqnNotePackManifest* merged)
{
    if (merged == nullptr || delta.cursor > protocol::v3::kMaxSafeJsonInteger) {
        return ESP_ERR_INVALID_ARG;
    }
    WqnNotePackManifest current;
    const esp_err_t load_result = LoadNotePackManifest(&current);
    if (load_result != ESP_OK && load_result != ESP_ERR_NOT_FOUND) {
        return load_result;
    }
    if (load_result == ESP_ERR_NOT_FOUND) {
        current = {};
    }
    // No monotonic guard on the cursor: it is a notebook-list offset (the sync
    // relists from 0 every cycle), not a change feed, so it legitimately moves
    // backwards between syncs and shrinks when notebooks are archived.

    for (const WqnNotePackManifestNotebook& change : delta.notebooks) {
        auto existing = std::find_if(
            current.notebooks.begin(),
            current.notebooks.end(),
            [&](const WqnNotePackManifestNotebook& item) {
                return item.notebook_id == change.notebook_id;
            });
        if (change.deleted) {
            if (existing != current.notebooks.end()) {
                current.notebooks.erase(existing);
            }
            continue;
        }
        if (change.notebook_id.size() != 36 ||
            (change.has_pack && change.pack_id.size() != 36)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (existing == current.notebooks.end()) {
            current.notebooks.push_back(change);
        } else {
            *existing = change;
        }
    }
    if (current.notebooks.size() > kMaxStoredNotebooks) {
        return ESP_ERR_INVALID_SIZE;
    }
    std::sort(
        current.notebooks.begin(),
        current.notebooks.end(),
        [](const WqnNotePackManifestNotebook& left, const WqnNotePackManifestNotebook& right) {
            return left.notebook_id < right.notebook_id;
        });
    current.cursor = delta.cursor;
    current.has_more = delta.has_more;
    *merged = std::move(current);
    return ESP_OK;
}

esp_err_t SaveNotePackManifestRaw(const WqnNotePackManifest& manifest)
{
    cJSON* root = cJSON_CreateObject();
    cJSON* data = cJSON_CreateObject();
    cJSON* notebooks = cJSON_CreateArray();
    if (root == nullptr || data == nullptr || notebooks == nullptr) {
        cJSON_Delete(root);
        cJSON_Delete(data);
        cJSON_Delete(notebooks);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddItemToObject(root, "data", data);
    cJSON_AddNumberToObject(data, "cursor", static_cast<double>(manifest.cursor));
    cJSON_AddBoolToObject(data, "has_more", manifest.has_more);
    cJSON_AddItemToObject(data, "notebooks", notebooks);

    for (const WqnNotePackManifestNotebook& item : manifest.notebooks) {
        cJSON* notebook = cJSON_CreateObject();
        if (notebook == nullptr) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToArray(notebooks, notebook);
        cJSON_AddStringToObject(notebook, "notebook_id", item.notebook_id.c_str());
        cJSON_AddStringToObject(notebook, "title", item.title.c_str());
        cJSON_AddNumberToObject(notebook, "change_sequence", static_cast<double>(item.change_sequence));
        cJSON_AddNumberToObject(notebook, "content_revision", static_cast<double>(item.content_revision));
        cJSON_AddBoolToObject(notebook, "has_pack", item.has_pack);
        if (!item.has_pack) {
            cJSON_AddNullToObject(notebook, "pack");
            continue;
        }
        cJSON* pack = cJSON_CreateObject();
        if (pack == nullptr) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToObject(notebook, "pack", pack);
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
        ESP_LOGW(kTag, "note pack manifest save fopen failed: %s", kManifestTempPath);
        cJSON_free(rendered);
        return ESP_FAIL;
    }
    const size_t length = std::strlen(rendered);
    const size_t written = std::fwrite(rendered, 1, length, file);
    const bool flushed = std::fflush(file) == 0 && ::fsync(fileno(file)) == 0;
    const bool closed = std::fclose(file) == 0;
    cJSON_free(rendered);
    if (written != length || !flushed || !closed) {
        ESP_LOGW(kTag, "note pack manifest durable write failed: want=%u got=%u",
                 static_cast<unsigned>(length), static_cast<unsigned>(written));
        std::remove(kManifestTempPath);
        return ESP_FAIL;
    }

    const bool had_primary = FileExists(kManifestPath);
    if (had_primary) {
        if (std::remove(kManifestBackupPath) != 0 && errno != ENOENT) {
            ESP_LOGW(kTag, "note pack manifest stale backup remove failed");
            std::remove(kManifestTempPath);
            return ESP_FAIL;
        }
        if (std::rename(kManifestPath, kManifestBackupPath) != 0) {
            ESP_LOGW(kTag, "note pack manifest backup commit failed");
            std::remove(kManifestTempPath);
            return ESP_FAIL;
        }
    }
    if (std::rename(kManifestTempPath, kManifestPath) != 0) {
        ESP_LOGW(kTag, "note pack manifest commit rename failed");
        if (had_primary && std::rename(kManifestBackupPath, kManifestPath) != 0) {
            ESP_LOGE(kTag, "note pack manifest rollback rename failed; backup remains readable");
        }
        std::remove(kManifestTempPath);
        return ESP_FAIL;
    }
    PruneUnreferencedNotePackFiles(manifest);
    return ESP_OK;
}

esp_err_t SaveNotePackManifestTransaction(void* opaque)
{
    return SaveNotePackManifestRaw(
        *static_cast<const WqnNotePackManifest*>(opaque));
}

esp_err_t SaveNotePackManifest(const WqnNotePackManifest& manifest)
{
    runtime::SleepLease storage_lease = runtime::SleepLease::TryAcquire(
        runtime::SleepBlocker::kStorage, "note-pack-manifest", __FILE__, __LINE__);
    if (!storage_lease) {
        return ESP_ERR_INVALID_STATE;
    }
    return services::ExecuteStorageTransaction(
        SaveNotePackManifestTransaction,
        const_cast<WqnNotePackManifest*>(&manifest));
}

esp_err_t LoadNotePackIndex(NotePackIndex* index)
{
    if (index == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    // SHA verification and JSONL scanning are CPU-bound; scope max frequency to
    // this rebuild instead of disabling dynamic frequency scaling globally.
    auto cpu_lease = runtime::CpuPerformanceLease::TryAcquire();
    *index = NotePackIndex{};
    index->mounted = InitNotePackStorage() == ESP_OK;
    if (!index->mounted) {
        index->status_message = "笔记分区不可用";
        return ESP_OK;
    }

    WqnNotePackManifest manifest;
    const esp_err_t manifest_result = LoadNotePackManifest(&manifest);
    if (manifest_result == ESP_ERR_NOT_FOUND) {
        index->status_message = "笔记未同步";
        return ESP_OK;
    }
    if (manifest_result != ESP_OK) {
        index->pack_error = true;
        index->status_message = "笔记清单损坏";
        return ESP_OK;
    }

    index->has_manifest = true;
    index->notebook_count = manifest.notebooks.size();

    size_t expected_entries = 0;
    size_t pack_notebooks = 0;
    for (const WqnNotePackManifestNotebook& item : manifest.notebooks) {
        if (item.notebook_id.size() != 36 ||
            (item.has_pack && item.sha256.size() != 64)) {
            index->pack_error = true;
            index->status_message = "笔记清单无效";
            ReleaseNotePackIndexAllocations(index);
            return ESP_OK;
        }
        if (!item.has_pack) {
            continue;
        }
        ++pack_notebooks;
        if (item.entry_count > kMaxIndexEntries - expected_entries) {
            index->pack_error = true;
            index->status_message = "笔记条目过多";
            ReleaseNotePackIndexAllocations(index);
            return ESP_OK;
        }
        expected_entries += item.entry_count;
    }

    const size_t entry_index_bytes = expected_entries * sizeof(NotePackIndexEntry);
    const size_t identity_bytes = pack_notebooks * sizeof(NotePackIdentity);
    const size_t required_psram = entry_index_bytes + identity_bytes;
    constexpr size_t kIndexAllocationReserveBytes = 64U * 1024U;
    const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t largest_psram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    if (required_psram > free_psram ||
        free_psram - required_psram < kIndexAllocationReserveBytes ||
        entry_index_bytes > largest_psram) {
        index->pack_error = true;
        index->status_message = "笔记索引内存不足";
        ESP_LOGW(
            kTag,
            "note pack index PSRAM preflight failed: need=%u entries=%u identities=%u free=%u largest=%u",
            static_cast<unsigned>(required_psram),
            static_cast<unsigned>(entry_index_bytes),
            static_cast<unsigned>(identity_bytes),
            static_cast<unsigned>(free_psram),
            static_cast<unsigned>(largest_psram));
        ReleaseNotePackIndexAllocations(index);
        return ESP_OK;
    }
    // Reserve the large PSRAM entries block first (the preflight validated it
    // fits the largest free block); the tiny identity/notebook reserves follow.
    index->entries.reserve(expected_entries);
    index->pack_identities.reserve(pack_notebooks);
    index->notebooks.reserve(manifest.notebooks.size());

    for (const WqnNotePackManifestNotebook& item : manifest.notebooks) {
        const uint32_t notebook_order = static_cast<uint32_t>(index->notebooks.size());
        NotePackNotebook notebook;
        notebook.notebook_id = item.notebook_id;
        notebook.title = item.title;
        notebook.content_revision = item.content_revision;
        notebook.pack_revision = item.pack_revision;
        notebook.sha256 = item.sha256;
        notebook.has_pack = item.has_pack;
        notebook.entry_begin = index->entries.size();
        notebook.entry_count = 0;

        if (item.has_pack) {
            NotePackIdentity identity;
            std::snprintf(identity.notebook_id, sizeof(identity.notebook_id), "%s", item.notebook_id.c_str());
            identity.content_revision = item.content_revision;
            identity.pack_revision = item.pack_revision;
            std::snprintf(identity.sha256, sizeof(identity.sha256), "%s", item.sha256.c_str());
            index->pack_identities.push_back(identity);

            const std::string path = PackPathForNotebook(item);
            struct stat st = {};
            if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                index->pack_bytes += static_cast<size_t>(st.st_size);
            }
            if (!VerifyFileSha256(path, item.sha256)) {
                index->pack_error = true;
                index->status_message = "笔记校验失败";
            } else {
                const esp_err_t scan_result = ScanNotePackFile(item, notebook_order, index);
                if (scan_result != ESP_OK) {
                    index->pack_error = true;
                    index->status_message = "笔记读取失败";
                } else {
                    notebook.entry_count = index->entries.size() - notebook.entry_begin;
                    index->manifest_revision =
                        std::max(index->manifest_revision, item.change_sequence);
                }
            }
        }
        index->notebooks.push_back(std::move(notebook));
    }

    // Build the note_id-sorted lookup once, after every notebook is scanned.
    // entries stay in display order; note_order holds indices sorted by note_id
    // so FindPackEntry can binary-search instead of scanning O(N) per lookup.
    index->note_order.reserve(index->entries.size());
    for (size_t i = 0; i < index->entries.size(); ++i) {
        index->note_order.push_back(static_cast<uint32_t>(i));
    }
    std::sort(
        index->note_order.begin(), index->note_order.end(),
        [&index](uint32_t a, uint32_t b) {
            return std::strcmp(index->entries[a].note_id, index->entries[b].note_id) < 0;
        });

    if (index->notebooks.empty() && index->status_message.empty()) {
        index->status_message = "暂无笔记本";
    } else if (index->status_message.empty()) {
        index->status_message = "笔记已就绪";
    }

    ESP_LOGI(
        kTag,
        "note pack index: notebooks=%u notes=%u pack_bytes=%u free_internal=%u free_psram=%u",
        static_cast<unsigned>(index->notebook_count),
        static_cast<unsigned>(index->entries.size()),
        static_cast<unsigned>(index->pack_bytes),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    // [note-image-diag] Image ids only reach the viewer through pack lines, so
    // report how many indexed notes carry attachments; with_images=0 while the
    // web shows attachments means the device pack predates the attach (sync
    // gap), not a UI bug.
    {
        unsigned with_images = 0;
        for (const auto& entry : index->entries) {
            if (entry.image_count > 0) ++with_images;
        }
        ESP_LOGI(
            kTag, "note pack index images: notes_with_images=%u", with_images);
    }
    return ESP_OK;
}

enum class NotePackStreamOperation : uint8_t {
    kBegin,
    kAppend,
    kCommit,
    kAbort,
};

struct NotePackStreamContext {
    const WqnNotePackManifestNotebook* notebook = nullptr;
    FILE* file = nullptr;
    mbedtls_sha256_context sha = {};
    bool sha_started = false;
    size_t bytes_written = 0;
    const uint8_t* chunk = nullptr;
    size_t chunk_size = 0;
    NotePackStreamOperation operation = NotePackStreamOperation::kBegin;
};

esp_err_t NotePackStreamTransaction(void* opaque)
{
    auto* context = static_cast<NotePackStreamContext*>(opaque);
    if (context == nullptr || context->notebook == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const std::string temp_path = TempPackPathForNotebook(*context->notebook);
    const std::string final_path = PackPathForNotebook(*context->notebook);
    switch (context->operation) {
        case NotePackStreamOperation::kBegin:
            std::remove(temp_path.c_str());
            context->file = std::fopen(temp_path.c_str(), "wb");
            if (context->file == nullptr) {
                ESP_LOGW(kTag, "note pack temp open failed: path=%s errno=%d", temp_path.c_str(), errno);
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

        case NotePackStreamOperation::kAppend:
            if (context->file == nullptr || !context->sha_started ||
                context->chunk == nullptr || context->chunk_size == 0 ||
                context->bytes_written + context->chunk_size > context->notebook->byte_size ||
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

        case NotePackStreamOperation::kCommit: {
            if (context->file == nullptr || !context->sha_started ||
                context->bytes_written != context->notebook->byte_size) {
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
            if (actual_sha != context->notebook->sha256) {
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

        case NotePackStreamOperation::kAbort:
            if (context->file != nullptr) {
                std::fclose(context->file);
                context->file = nullptr;
            }
            std::remove(temp_path.c_str());
            return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t AppendNotePackStream(void* opaque, const uint8_t* bytes, size_t size)
{
    auto* context = static_cast<NotePackStreamContext*>(opaque);
    if (context == nullptr || bytes == nullptr || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    context->operation = NotePackStreamOperation::kAppend;
    context->chunk = bytes;
    context->chunk_size = size;
    return services::ExecuteStorageTransaction(NotePackStreamTransaction, context);
}

esp_err_t DownloadNotePackToStorage(
    const std::string& token,
    const protocol::v3::RequestMetadata& metadata,
    const WqnNotePackManifestNotebook& notebook,
    WqnTransferProgressSink progress)
{
    if (!notebook.has_pack || notebook.pack_id.size() != 36 ||
        notebook.sha256.size() != 64 ||
        notebook.schema_version != kPackSchemaVersion ||
        notebook.entry_count > kMaxPackEntries || notebook.byte_size == 0 ||
        notebook.byte_size > kMaxPackBytes) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!NotePackNeedsDownload(notebook)) {
        return ESP_OK;
    }
    runtime::SleepLease storage_lease = runtime::SleepLease::TryAcquire(
        runtime::SleepBlocker::kStorage, "note-pack-stream", __FILE__, __LINE__);
    if (!storage_lease) {
        return ESP_ERR_INVALID_STATE;
    }
    NotePackStreamContext context;
    context.notebook = &notebook;
    mbedtls_sha256_init(&context.sha);
    context.operation = NotePackStreamOperation::kBegin;
    esp_err_t result = services::ExecuteStorageTransaction(
        NotePackStreamTransaction, &context);
    if (result == ESP_OK) {
        result = DownloadNotePackStream(
            token, metadata, notebook, AppendNotePackStream, &context, progress);
    }
    if (result == ESP_OK) {
        context.operation = NotePackStreamOperation::kCommit;
        result = services::ExecuteStorageTransaction(
            NotePackStreamTransaction, &context);
    }
    if (result != ESP_OK) {
        context.operation = NotePackStreamOperation::kAbort;
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            services::ExecuteStorageTransaction(NotePackStreamTransaction, &context));
    }
    mbedtls_sha256_free(&context.sha);
    return result;
}

bool NotePackNeedsDownload(
    const WqnNotePackManifestNotebook& notebook,
    const std::string* verified_sha)
{
    if (!notebook.has_pack) {
        return false;
    }
    const std::string path = PackPathForNotebook(notebook);
    struct stat status = {};
    if (stat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode)) {
        return true;
    }
    if (verified_sha != nullptr && *verified_sha == notebook.sha256 &&
        static_cast<uint64_t>(status.st_size) == notebook.byte_size) {
        // The listing matches the sha we verified at download time and the
        // file is size-intact: skip the full-file re-hash.
        return false;
    }
    return !VerifyFileSha256(path, notebook.sha256);
}

esp_err_t ReadNotePackEntry(const NotePackIndexEntry& index_entry, WqnNoteEntry* entry)
{
    if (entry == nullptr || index_entry.pack_stem[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    // A note open performs a bounded SPIFFS seek, JSONL read and cJSON parse;
    // keep it off the 40 MHz DFS floor as well.
    auto cpu_lease = runtime::CpuPerformanceLease::TryAcquire();
    *entry = WqnNoteEntry{};

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
    const esp_err_t line_result = ReadBoundedNotePackLine(file, &line_buffer, &line);
    std::fclose(file);
    if (line_result != ESP_OK) {
        return line_result;
    }
    const esp_err_t parse_result = ParseNoteRecordLine(line.c_str(), entry, /*include_content=*/true);
    if (parse_result != ESP_OK) {
        return parse_result;
    }
    entry->note_id = index_entry.note_id;
    entry->notebook_id = index_entry.notebook_id;
    return ESP_OK;
}

namespace {

std::string NoteImageCachePath(const std::string& image_id)
{
    return std::string(kStorageRoot) + "/ni_" + image_id.substr(0, 12) + ".wqni";
}

bool IsNoteImageId(const std::string& image_id)
{
    if (image_id.size() != 64) return false;
    for (char c : image_id) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

// Keeps the image cache bounded: when at/over the cap, delete the
// oldest-mtime ni_*.wqni files until one slot is free. SPIFFS mtimes are
// second-granular; ties just pick an arbitrary victim, which is fine for a
// best-effort LRU.
void EvictNoteImageCacheIfNeeded()
{
    // First pass: names only. stat() on SPIFFS is a full node lookup, so the
    // common case (cache below the cap) must not pay 64 of them on the
    // storage task while foreground transactions queue behind it.
    std::vector<std::string> names;
    DIR* directory = opendir(kStorageRoot);
    if (directory == nullptr) return;
    struct dirent* item = nullptr;
    while ((item = readdir(directory)) != nullptr) {
        const std::string name = item->d_name;
        if (name.rfind("ni_", 0) != 0 || name.size() < 8 ||
            name.substr(name.size() - 5) != ".wqni") {
            continue;
        }
        names.push_back(std::string(kStorageRoot) + "/" + name);
    }
    closedir(directory);
    if (names.size() < wqn::kNoteImageCacheMaxFiles) return;

    struct Candidate {
        std::string path;
        time_t mtime;
    };
    std::vector<Candidate> files;
    files.reserve(names.size());
    for (const std::string& path : names) {
        struct stat info = {};
        files.push_back({path, stat(path.c_str(), &info) == 0 ? info.st_mtime : 0});
    }
    std::sort(files.begin(), files.end(), [](const Candidate& a, const Candidate& b) {
        return a.mtime < b.mtime;
    });
    const size_t to_delete = files.size() + 1 - wqn::kNoteImageCacheMaxFiles;
    for (size_t i = 0; i < to_delete && i < files.size(); ++i) {
        if (unlink(files[i].path.c_str()) != 0) {
            ESP_LOGW(kTag, "note image cache eviction failed: %s", files[i].path.c_str());
        }
    }
}

struct StoreNoteImageContext {
    const std::string* image_id = nullptr;
    const uint8_t* data = nullptr;
    size_t size = 0;
};

struct LoadNoteImageContext {
    const std::string* image_id = nullptr;
    std::vector<uint8_t>* wqni = nullptr;
};

esp_err_t LoadNoteImageTransaction(void* opaque)
{
    auto* context = static_cast<LoadNoteImageContext*>(opaque);
    if (context == nullptr || context->image_id == nullptr ||
        context->wqni == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const std::string path = NoteImageCachePath(*context->image_id);
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }
    // Read the header first so the cache accepts either BW1 or GRAY4 without
    // allocating a second fixed-size buffer.
    std::array<uint8_t, kNoteImageHeaderBytes> header = {};
    const size_t header_read = std::fread(header.data(), 1, header.size(), file);
    if (header_read != header.size()) {
        std::fclose(file);
        unlink(path.c_str());
        return ESP_ERR_INVALID_RESPONSE;
    }
    const uint8_t format = header[5];
    const size_t payload = format == 1 ? kNoteImagePayloadBytes
        : format == 2 ? kNoteImageGray4PayloadBytes : 0;
    if (payload == 0) {
        std::fclose(file);
        unlink(path.c_str());
        return ESP_ERR_INVALID_RESPONSE;
    }
    context->wqni->assign(kNoteImageHeaderBytes + payload, 0);
    std::memcpy(context->wqni->data(), header.data(), header.size());
    const size_t read = std::fread(
        context->wqni->data() + kNoteImageHeaderBytes, 1, payload, file);
    const bool extra = std::fgetc(file) != EOF;
    std::fclose(file);
    if (read != payload || extra ||
        ValidateNoteImageWqni(context->wqni->data(), context->wqni->size()) != ESP_OK) {
        // Corrupt cache entries are dropped so the next request re-downloads.
        unlink(path.c_str());
        context->wqni->clear();
        return ESP_ERR_INVALID_CRC;
    }
    // Refresh mtime on a successful hit so capacity reclamation is genuinely
    // least-recently-used rather than oldest-created.
    struct utimbuf touched = {};
    touched.actime = std::time(nullptr);
    touched.modtime = touched.actime;
    if (utime(path.c_str(), &touched) != 0) {
        ESP_LOGD(kTag, "note image cache touch failed: %s", path.c_str());
    }
    return ESP_OK;
}

esp_err_t StoreNoteImageTransaction(void* opaque)
{
    const auto* context = static_cast<const StoreNoteImageContext*>(opaque);
    if (context == nullptr || context->image_id == nullptr ||
        context->data == nullptr || context->size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    EvictNoteImageCacheIfNeeded();
    const std::string path = NoteImageCachePath(*context->image_id);
    const std::string temp = path + ".tmp";
    FILE* file = std::fopen(temp.c_str(), "wb");
    if (file == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    const size_t written = std::fwrite(context->data, 1, context->size, file);
    const bool flushed = std::fflush(file) == 0 && ::fsync(fileno(file)) == 0;
    const bool closed = std::fclose(file) == 0;
    if (written != context->size || !flushed || !closed) {
        unlink(temp.c_str());
        return ESP_FAIL;
    }
    unlink(path.c_str());
    if (rename(temp.c_str(), path.c_str()) != 0) {
        unlink(temp.c_str());
        return ESP_FAIL;
    }
    return ESP_OK;
}

}  // namespace

esp_err_t ValidateNoteImageWqni(const uint8_t* data, size_t size)
{
    if (data == nullptr || size < kNoteImageHeaderBytes) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (std::memcmp(data, "WQNI", 4) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const uint8_t version = data[4];
    const uint8_t pixel_format = data[5];
    const uint16_t flags = static_cast<uint16_t>(data[6]) | (static_cast<uint16_t>(data[7]) << 8);
    const uint16_t width = static_cast<uint16_t>(data[8]) | (static_cast<uint16_t>(data[9]) << 8);
    const uint16_t height = static_cast<uint16_t>(data[10]) | (static_cast<uint16_t>(data[11]) << 8);
    uint32_t payload_length = 0;
    uint32_t crc = 0;
    std::memcpy(&payload_length, data + 12, sizeof(payload_length));
    std::memcpy(&crc, data + 16, sizeof(crc));
    // flags 0x0003 = MSB-first bit order + 1-renders-white: the exact wqn_epd
    // framebuffer convention. Anything else would need a transform we do not do.
    const size_t expected_payload = pixel_format == 1
        ? kNoteImagePayloadBytes
        : pixel_format == 2 ? kNoteImageGray4PayloadBytes : 0;
    if (version != 1 || flags != 0x0003 || width != 400 || height != 300 ||
        expected_payload == 0 || payload_length != expected_payload ||
        size != kNoteImageHeaderBytes + expected_payload) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const uint32_t actual = esp_rom_crc32_le(
        0, data + kNoteImageHeaderBytes, expected_payload);
    if (actual != crc) {
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

esp_err_t LoadCachedNoteImage(const std::string& image_id, std::vector<uint8_t>* wqni)
{
    if (wqni == nullptr || !IsNoteImageId(image_id)) {
        return ESP_ERR_INVALID_ARG;
    }
    runtime::SleepLease storage_lease = runtime::SleepLease::TryAcquire(
        runtime::SleepBlocker::kStorage,
        "note-image-read",
        __FILE__,
        __LINE__);
    if (!storage_lease) {
        return ESP_ERR_INVALID_STATE;
    }
    LoadNoteImageContext context = {&image_id, wqni};
    return services::ExecuteStorageTransaction(LoadNoteImageTransaction, &context);
}

esp_err_t StoreCachedNoteImage(const std::string& image_id, const uint8_t* data, size_t size)
{
    if (!IsNoteImageId(image_id)) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t valid = ValidateNoteImageWqni(data, size);
    if (valid != ESP_OK) {
        return valid;
    }
    runtime::SleepLease storage_lease = runtime::SleepLease::TryAcquire(
        runtime::SleepBlocker::kStorage,
        "note-image-cache",
        __FILE__,
        __LINE__);
    if (!storage_lease) {
        return ESP_ERR_INVALID_STATE;
    }
    StoreNoteImageContext context = {&image_id, data, size};
    return services::ExecuteStorageTransaction(
        StoreNoteImageTransaction,
        &context);
}

}  // namespace wqn
