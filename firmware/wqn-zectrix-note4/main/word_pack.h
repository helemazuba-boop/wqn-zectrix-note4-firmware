#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "esp_err.h"
#include "wqn_api.h"

namespace wqn {

struct WordPackIndexEntry {
    std::string pack_id;
    std::string word_id;
    std::string deck_id;
    std::string word;
    std::string normalized_word;
    std::string status;
    long file_offset = 0;
};

struct WordPackIndex {
    bool mounted = false;
    bool has_manifest = false;
    bool truncated = false;
    bool pack_error = false;
    std::string status_message;
    size_t pack_count = 0;
    size_t pack_bytes = 0;
    std::vector<WordPackIndexEntry> entries;
};

esp_err_t InitWordPackStorage();
esp_err_t SaveWordPackManifest(const WqnWordPackManifest& manifest);
esp_err_t LoadWordPackIndex(WordPackIndex* index);
esp_err_t SaveWordPackFromBytes(const WqnWordPackManifestItem& item, const std::string& bytes);
bool WordPackNeedsDownload(const WqnWordPackManifestItem& item);
esp_err_t ReadWordPackEntry(const WordPackIndexEntry& index_entry, WqnWordEntry* entry);
void FindWordPackPrefixMatches(const WordPackIndex& index, const std::string& prefix, size_t limit, std::vector<size_t>* matches);
std::vector<char> WordPackNextLetters(const WordPackIndex& index, const std::string& prefix);
std::string NormalizeWordLookupText(const std::string& value);

}  // namespace wqn
