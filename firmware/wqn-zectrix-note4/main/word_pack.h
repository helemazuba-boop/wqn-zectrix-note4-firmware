#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "wqn_api.h"

namespace wqn {

// [mem-fix] The word index is large (3500+ entries, each with several short
// strings -> ~1.4 MB). CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=512 forces every
// small malloc into the ~298 KB internal RAM, so std::string-backed entries
// exhausted internal heap at ~1000 words (index truncated) and left the device
// out of memory (review/render crash). This allocator pins the word index — the
// only large, long-lived structure here — into the 8 MB PSRAM pool instead.
template <typename T>
struct PsramAllocator {
    using value_type = T;
    PsramAllocator() noexcept = default;
    template <typename U>
    PsramAllocator(const PsramAllocator<U>&) noexcept {}

    T* allocate(std::size_t n) {
        void* p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM);
        if (p == nullptr) {
            // Project builds with -fno-exceptions, so we cannot throw
            // std::bad_alloc. PSRAM is 8 MB vs an index of ~1.4 MB, so this
            // only fires on genuine exhaustion; abort rather than corrupt.
            abort();
        }
        return static_cast<T*>(p);
    }
    void deallocate(T* p, std::size_t) noexcept { heap_caps_free(p); }
};

template <typename A, typename B>
bool operator==(const PsramAllocator<A>&, const PsramAllocator<B>&) noexcept {
    return true;
}
template <typename A, typename B>
bool operator!=(const PsramAllocator<A>&, const PsramAllocator<B>&) noexcept {
    return false;
}

// PSRAM-backed, fixed-size POD index entry. Deliberately NOT std::string:
// a std::basic_string with a custom PSRAM allocator corrupted the heap during
// vector growth/move (SSO interactions), crashing in tlsf_walk_pool. A trivially
// copyable POD is memcpy/realloc-safe in the PSRAM vector and also far smaller
// (~128 B/entry vs ~400 B). Fields the review/dictionary flow doesn't need
// (word_id / deck_id / status) are dropped; ReadWordPackEntry re-parses the full
// entry from the pack file anyway. Strings are length-capped; over-long words are
// truncated (rare; only affects display/prefix-match of >47-byte tokens).
struct WordPackIndexEntry {
    char word[48];
    char normalized_word[48];
    char pack_stem[28];
    uint32_t file_offset;
};

struct WordPackIndex {
    bool mounted = false;
    bool has_manifest = false;
    bool truncated = false;
    bool pack_error = false;
    std::string status_message;
    size_t pack_count = 0;
    size_t pack_bytes = 0;
    std::vector<WordPackIndexEntry, PsramAllocator<WordPackIndexEntry>> entries;
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
std::string SafePackStemFromId(const std::string& pack_id);

}  // namespace wqn
