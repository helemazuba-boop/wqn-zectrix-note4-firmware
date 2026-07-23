#include "word_pack.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "mbedtls/sha256.h"
#include "runtime/sleep_coordinator.h"
#include "services/storage_service.h"

namespace {

constexpr char kTag[] = "word_pack";
constexpr char kStorageRoot[] = "/storage";
constexpr char kManifestPath[] = "/storage/wp_manifest.json";
constexpr char kManifestTempPath[] = "/storage/wp_manifest.tmp";
constexpr char kManifestBackupPath[] = "/storage/wp_manifest.bak";
constexpr char kPackMagic[] = "WQN_WORD_PACK_V1";
constexpr size_t kMaxIndexEntries = 10000;
constexpr size_t kLineBufferSize = 4096;

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

int GetOptionalInt(cJSON* object, const char* key)
{
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(item) ? item->valueint : 0;
}

void TrimLineEnding(std::string* value)
{
    if (value == nullptr) {
        return;
    }
    while (!value->empty() && (value->back() == '\n' || value->back() == '\r')) {
        value->pop_back();
    }
}

bool FileExists(const std::string& path)
{
    struct stat st = {};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

}  // namespace

namespace wqn {

std::string SafePackStem(const WqnWordPackManifestItem& item)
{
    std::string stem;
    stem.reserve(25);
    for (const char ch : item.pack_id) {
        const bool keep = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        if (keep) {
            stem.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
        if (stem.size() >= 8) {
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
                if (++hash_chars >= 16) {
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

std::string HexSha256(const uint8_t* data, size_t size)
{
    std::array<unsigned char, 32> digest = {};
    mbedtls_sha256(data, size, digest.data(), 0);

    constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const unsigned char byte : digest) {
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0F]);
    }
    return out;
}

std::string HexSha256(const std::string& bytes)
{
    return HexSha256(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
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

void ParsePackEntryLine(const char* line, wqn::WqnWordEntry* entry)
{
    if (line == nullptr || entry == nullptr) {
        return;
    }
    JsonDocument document(line);
    if (!document.ok() || !cJSON_IsObject(document.root())) {
        return;
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
    entry->revision = GetOptionalInt(document.root(), "revision");
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
    wqn::WordPackIndex* index)
{
    const std::string path = PackPathForItem(item);
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }

    // [stack-fix] 4 KB on the stack + the deep fgets->SPIFFS->esp_partition_read
    // ->tlsf_walk_pool call chain overflowed the 8 KB task stack and corrupted
    // the adjacent heap (crash in tlsf_walk_pool reading a smashed block header).
    // Keep this large buffer off the stack.
    std::vector<char> line(kLineBufferSize, 0);
    if (std::fgets(line.data(), line.size(), file) == nullptr) {
        std::fclose(file);
        return ESP_FAIL;
    }
    std::string header = line.data();
    TrimLineEnding(&header);
    if (header != kPackMagic) {
        std::fclose(file);
        return ESP_ERR_INVALID_STATE;
    }

    // [mem-fix] Reserve up front so the PSRAM-backed vector doesn't realloc +
    // copy ~12 times while ingesting thousands of entries. entry_count comes
    // from the manifest; add it to the existing size (multiple packs accumulate
    // into one index) and clamp to the hard cap.
    if (item.entry_count > 0) {
        size_t target = index->entries.size() + static_cast<size_t>(item.entry_count);
        if (target > kMaxIndexEntries) {
            target = kMaxIndexEntries;
        }
        if (target > index->entries.capacity()) {
            index->entries.reserve(target);
        }
    }

    if (std::fgets(line.data(), line.size(), file) == nullptr) {
        std::fclose(file);
        return ESP_FAIL;
    }

    // Pack stem is identical for every entry in this file; compute once.
    const std::string pack_stem = wqn::SafePackStem(item);

    while (index->entries.size() < kMaxIndexEntries) {
        const long offset = std::ftell(file);
        if (std::fgets(line.data(), line.size(), file) == nullptr) {
            break;
        }
        wqn::WqnWordEntry entry;
        ParsePackEntryLine(line.data(), &entry);
        if (entry.word.empty()) {
            continue;
        }

        wqn::WordPackIndexEntry indexed = {};
        CopyField(indexed.word, sizeof(indexed.word), entry.word);
        const std::string& normalized =
            !entry.normalized_word.empty() ? entry.normalized_word : wqn::NormalizeWordLookupText(entry.word);
        CopyField(indexed.normalized_word, sizeof(indexed.normalized_word), normalized);
        CopyField(indexed.pack_stem, sizeof(indexed.pack_stem), pack_stem);
        indexed.file_offset = static_cast<uint32_t>(offset);
        index->entries.push_back(indexed);
    }
    if (!std::feof(file)) {
        index->truncated = true;
        // [trunc-warn] Log when the pack exceeds the hard cap so it's not silent
        ESP_LOGW(kTag, "word pack truncated at kMaxIndexEntries=%zu (pack=%s) - excess entries dropped", kMaxIndexEntries, item.pack_id.c_str());
    }
    std::fclose(file);
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
        cJSON_AddStringToObject(pack, "revision", item.revision.c_str());
        cJSON_AddStringToObject(pack, "schema_version", item.schema_version.c_str());
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

esp_err_t LoadWordPackIndex(WordPackIndex* index)
{
    if (index == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
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
    index->has_manifest = true;
    index->pack_count = manifest.packs.size();

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
        const esp_err_t scan_result = ScanPackFile(item, index);
        if (scan_result != ESP_OK) {
            index->pack_error = true;
            index->status_message = "词库读取失败";
        }
    }

    // [sort-fix] Sort entries globally by normalized_word. Each pack file is
    // individually sorted, but multi-pack concatenation breaks the global
    // ordering that FindWordPackPrefixMatches / WordPackNextLetters rely on
    // for their early-break optimization (strncmp > 0 -> break). Without
    // this sort, the dictionary may silently miss matches from later packs.
    if (index->entries.size() > 1) {
        std::sort(index->entries.begin(), index->entries.end(),
                  [](const WordPackIndexEntry& a, const WordPackIndexEntry& b) {
                      return std::strcmp(a.normalized_word, b.normalized_word) < 0;
                  });
    }

    if (index->entries.empty() && index->status_message.empty()) {
        index->status_message = "词库为空";
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

esp_err_t SaveWordPackFromBytesRaw(const WqnWordPackManifestItem& item, const std::string& bytes)
{
    if (item.pack_id.empty() || item.sha256.empty() || bytes.empty()) {
        return ESP_ERR_INVALID_ARG;
    }
    if (item.byte_size > 0 && static_cast<int>(bytes.size()) != item.byte_size) {
        ESP_LOGW(kTag, "word pack size mismatch: expected=%d actual=%u", item.byte_size, static_cast<unsigned>(bytes.size()));
        return ESP_ERR_INVALID_SIZE;
    }
    const std::string actual_sha = HexSha256(bytes);
    if (actual_sha != item.sha256) {
        ESP_LOGW(kTag, "word pack sha mismatch: pack_id=%s", item.pack_id.c_str());
        return ESP_ERR_INVALID_CRC;
    }

    const std::string final_path = PackPathForItem(item);
    if (VerifyFileSha256(final_path, item.sha256)) {
        return ESP_OK;
    }

    const std::string tmp_path = TempPackPathForItem(item);
    FILE* file = std::fopen(tmp_path.c_str(), "wb");
    if (file == nullptr) {
        ESP_LOGW(kTag, "word pack save fopen failed: pack_id=%s path=%s", item.pack_id.c_str(), tmp_path.c_str());
        return ESP_FAIL;
    }
    const size_t written = std::fwrite(bytes.data(), 1, bytes.size(), file);
    const bool flushed = std::fflush(file) == 0 && ::fsync(fileno(file)) == 0;
    const bool closed = std::fclose(file) == 0;
    if (written != bytes.size() || !flushed || !closed) {
        ESP_LOGW(kTag, "word pack durable write failed: pack_id=%s want=%u got=%u",
                 item.pack_id.c_str(), static_cast<unsigned>(bytes.size()), static_cast<unsigned>(written));
        std::remove(tmp_path.c_str());
        return ESP_FAIL;
    }

    if (FileExists(final_path) && std::remove(final_path.c_str()) != 0) {
        ESP_LOGW(kTag, "word pack invalid destination remove failed: pack_id=%s", item.pack_id.c_str());
        std::remove(tmp_path.c_str());
        return ESP_FAIL;
    }
    if (std::rename(tmp_path.c_str(), final_path.c_str()) != 0) {
        ESP_LOGW(kTag, "word pack rename failed: pack_id=%s %s -> %s",
                 item.pack_id.c_str(), tmp_path.c_str(), final_path.c_str());
        std::remove(tmp_path.c_str());
        return ESP_FAIL;
    }
    return ESP_OK;
}

struct WordPackWriteContext {
    const WqnWordPackManifestItem* item;
    const std::string* bytes;
};

esp_err_t SaveWordPackBytesTransaction(void* opaque)
{
    const auto* context = static_cast<const WordPackWriteContext*>(opaque);
    return SaveWordPackFromBytesRaw(*context->item, *context->bytes);
}

esp_err_t SaveWordPackFromBytes(const WqnWordPackManifestItem& item, const std::string& bytes)
{
    runtime::SleepLease storage_lease = runtime::SleepLease::TryAcquire(
        runtime::SleepBlocker::kStorage, "word-pack-file", __FILE__, __LINE__);
    if (!storage_lease) {
        return ESP_ERR_INVALID_STATE;
    }
    WordPackWriteContext context = {&item, &bytes};
    return services::ExecuteStorageTransaction(
        SaveWordPackBytesTransaction,
        &context);
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
    std::vector<char> line(kLineBufferSize, 0);
    if (std::fgets(line.data(), line.size(), file) == nullptr) {
        std::fclose(file);
        return ESP_FAIL;
    }
    std::fclose(file);

    ParsePackEntryLine(line.data(), entry);
    return entry->word.empty() ? ESP_FAIL : ESP_OK;
}

void FindWordPackPrefixMatches(const WordPackIndex& index, const std::string& prefix, size_t limit, std::vector<size_t>* matches)
{
    if (matches == nullptr) {
        return;
    }
    matches->clear();
    const std::string normalized = NormalizeWordLookupText(prefix);
    // [dict-perf] Pack entries are sorted by normalized_word (cloud-side
    // loadDeckEntries orders by sort_index, normalized_word). Once we pass
    // the prefix range (strncmp > 0), all subsequent entries are also past
    // the range, so we can break early instead of scanning all 3500+.
    for (size_t i = 0; i < index.entries.size(); ++i) {
        if (!normalized.empty()) {
            const int cmp = std::strncmp(index.entries[i].normalized_word,
                                         normalized.c_str(), normalized.size());
            if (cmp < 0) {
                continue;  // before prefix range
            }
            if (cmp > 0) {
                break;  // past prefix range, done
            }
        }
        matches->push_back(i);
        if (matches->size() >= limit) {
            break;
        }
    }
}

std::vector<char> WordPackNextLetters(const WordPackIndex& index, const std::string& prefix)
{
    const std::string normalized = NormalizeWordLookupText(prefix);
    std::vector<char> letters;
    // [dict-perf] Same early-break optimization as FindWordPackPrefixMatches:
    // entries are sorted, so once strncmp > 0 we've passed the prefix range.
    for (const WordPackIndexEntry& entry : index.entries) {
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
