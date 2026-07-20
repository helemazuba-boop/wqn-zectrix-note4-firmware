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

struct PersistedWordSession;

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
// copyable POD is memcpy/realloc-safe in the PSRAM vector. W3 retains stable
// item/deck IDs and import ordering so sessions can pin exact content while a
// separate compact dictionary index provides lexicographic lookup.
struct WordPackIndexEntry {
    char word_id[37];
    char deck_id[37];
    char word[81];
    char normalized_word[81];
    char pack_stem[28];
    uint32_t file_offset;
    uint32_t deck_order;
    int32_t sort_index;
};

struct WordPackIdentity {
    char deck_id[37] = {};
    char title[65] = {};
    uint64_t content_revision = 0;
    uint64_t pack_revision = 0;
    char sha256[65] = {};
};

struct WordPackIndex {
    bool mounted = false;
    bool has_manifest = false;
    bool truncated = false;
    bool pack_error = false;
    std::string status_message;
    size_t pack_count = 0;
    size_t pack_bytes = 0;
    uint64_t manifest_revision = 0;
    std::vector<WordPackIndexEntry, PsramAllocator<WordPackIndexEntry>> entries;
    std::vector<uint32_t, PsramAllocator<uint32_t>> dictionary_order;
    std::vector<WordPackIdentity, PsramAllocator<WordPackIdentity>> pack_identities;
};

esp_err_t InitWordPackStorage();
esp_err_t ResetWordPackStorageCache();
esp_err_t LoadWordPackManifest(WqnWordPackManifest* manifest);
esp_err_t MergeWordPackManifestDelta(
    const WqnWordPackManifest& delta,
    WqnWordPackManifest* merged);
esp_err_t SaveWordPackManifest(const WqnWordPackManifest& manifest);
esp_err_t LoadWordPackIndex(WordPackIndex* index);
esp_err_t LoadWordPackIndexForSession(
    const PersistedWordSession& session,
    WordPackIndex* index);
bool WordPackIndexMatchesSession(
    const WordPackIndex& index,
    const PersistedWordSession& session);
esp_err_t DownloadWordPackToStorage(
    const std::string& token,
    const protocol::v3::RequestMetadata& metadata,
    const WqnWordPackManifestItem& item);
bool WordPackNeedsDownload(const WqnWordPackManifestItem& item);
esp_err_t ReadWordPackEntry(const WordPackIndexEntry& index_entry, WqnWordEntry* entry);
void FindWordPackPrefixMatches(const WordPackIndex& index, const std::string& prefix, size_t limit, std::vector<size_t>* matches);
std::vector<char> WordPackNextLetters(const WordPackIndex& index, const std::string& prefix);
std::string NormalizeWordLookupText(const std::string& value);

}  // namespace wqn
