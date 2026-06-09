#include "word_pack.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"

namespace {

constexpr char kTag[] = "word_pack";
constexpr char kStorageRoot[] = "/storage";
constexpr char kManifestPath[] = "/storage/wp_manifest.json";
constexpr char kPackMagic[] = "WQN_WORD_PACK_V1";
constexpr size_t kMaxIndexEntries = 10000;
constexpr size_t kLineBufferSize = 3072;

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

std::string SafePackStem(const wqn::WqnWordPackManifestItem& item)
{
    std::string source = !item.pack_id.empty() ? item.pack_id : item.sha256;
    std::string stem;
    stem.reserve(16);
    for (const char ch : source) {
        const bool keep = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        if (keep) {
            stem.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
        if (stem.size() >= 16) {
            break;
        }
    }
    if (stem.empty()) {
        stem = "default";
    }
    return stem;
}

std::string SafePackStemFromId(const std::string& pack_id)
{
    wqn::WqnWordPackManifestItem item;
    item.pack_id = pack_id;
    return SafePackStem(item);
}

std::string PackPathForStem(const std::string& stem)
{
    return std::string(kStorageRoot) + "/wp_" + stem + ".wqwp";
}

std::string PackPathForItem(const wqn::WqnWordPackManifestItem& item)
{
    return PackPathForStem(SafePackStem(item));
}

std::string TempPackPathForItem(const wqn::WqnWordPackManifestItem& item)
{
    return std::string(kStorageRoot) + "/wp_" + SafePackStem(item) + ".tmp";
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
    const esp_err_t result = ReadWholeFile(kManifestPath, &payload);
    if (result != ESP_OK) {
        return result;
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

esp_err_t ScanPackFile(
    const wqn::WqnWordPackManifestItem& item,
    wqn::WordPackIndex* index)
{
    const std::string path = PackPathForItem(item);
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }

    std::array<char, kLineBufferSize> line = {};
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

    if (std::fgets(line.data(), line.size(), file) == nullptr) {
        std::fclose(file);
        return ESP_FAIL;
    }

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

        wqn::WordPackIndexEntry indexed;
        indexed.pack_id = item.pack_id;
        indexed.word_id = entry.id;
        indexed.deck_id = !entry.deck_id.empty() ? entry.deck_id : item.deck_id;
        indexed.word = entry.word;
        indexed.normalized_word = !entry.normalized_word.empty() ? entry.normalized_word : wqn::NormalizeWordLookupText(entry.word);
        indexed.status = entry.status;
        indexed.file_offset = offset;
        index->entries.push_back(std::move(indexed));
    }
    if (!std::feof(file)) {
        index->truncated = true;
    }
    std::fclose(file);
    return ESP_OK;
}

}  // namespace

namespace wqn {

esp_err_t InitWordPackStorage()
{
    struct stat st = {};
    if (stat(kStorageRoot, &st) != 0 || !S_ISDIR(st.st_mode)) {
        ESP_LOGW(kTag, "storage SPIFFS mount point is not available");
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t SaveWordPackManifest(const WqnWordPackManifest& manifest)
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

    FILE* file = std::fopen(kManifestPath, "wb");
    if (file == nullptr) {
        cJSON_free(rendered);
        return ESP_FAIL;
    }
    const size_t length = std::strlen(rendered);
    const size_t written = std::fwrite(rendered, 1, length, file);
    std::fclose(file);
    cJSON_free(rendered);
    return written == length ? ESP_OK : ESP_FAIL;
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

esp_err_t SaveWordPackFromBytes(const WqnWordPackManifestItem& item, const std::string& bytes)
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

    const std::string tmp_path = TempPackPathForItem(item);
    FILE* file = std::fopen(tmp_path.c_str(), "wb");
    if (file == nullptr) {
        return ESP_FAIL;
    }
    const size_t written = std::fwrite(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    if (written != bytes.size()) {
        std::remove(tmp_path.c_str());
        return ESP_FAIL;
    }

    const std::string final_path = PackPathForItem(item);
    std::remove(final_path.c_str());
    if (std::rename(tmp_path.c_str(), final_path.c_str()) != 0) {
        std::remove(tmp_path.c_str());
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool WordPackNeedsDownload(const WqnWordPackManifestItem& item)
{
    const std::string path = PackPathForItem(item);
    return !FileExists(path) || !VerifyFileSha256(path, item.sha256);
}

esp_err_t ReadWordPackEntry(const WordPackIndexEntry& index_entry, WqnWordEntry* entry)
{
    if (entry == nullptr || index_entry.pack_id.empty() || index_entry.file_offset < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *entry = WqnWordEntry{};

    const std::string path = PackPathForStem(SafePackStemFromId(index_entry.pack_id));
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }
    if (std::fseek(file, index_entry.file_offset, SEEK_SET) != 0) {
        std::fclose(file);
        return ESP_FAIL;
    }
    std::array<char, kLineBufferSize> line = {};
    if (std::fgets(line.data(), line.size(), file) == nullptr) {
        std::fclose(file);
        return ESP_FAIL;
    }
    std::fclose(file);

    ParsePackEntryLine(line.data(), entry);
    if (entry->deck_id.empty()) {
        entry->deck_id = index_entry.deck_id;
    }
    if (entry->id.empty()) {
        entry->id = index_entry.word_id;
    }
    return entry->word.empty() ? ESP_FAIL : ESP_OK;
}

void FindWordPackPrefixMatches(const WordPackIndex& index, const std::string& prefix, size_t limit, std::vector<size_t>* matches)
{
    if (matches == nullptr) {
        return;
    }
    matches->clear();
    const std::string normalized = NormalizeWordLookupText(prefix);
    for (size_t i = 0; i < index.entries.size(); ++i) {
        if (!normalized.empty() && index.entries[i].normalized_word.rfind(normalized, 0) != 0) {
            continue;
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
    for (const WordPackIndexEntry& entry : index.entries) {
        if (entry.normalized_word.size() <= normalized.size()) {
            continue;
        }
        if (!normalized.empty() && entry.normalized_word.rfind(normalized, 0) != 0) {
            continue;
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
