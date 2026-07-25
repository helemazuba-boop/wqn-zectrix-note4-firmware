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

// [mem-fix] The note index groups every synced note across all notebooks. Each
// entry is a fixed-size POD (~240 bytes) and thousands can accumulate, so the
// index vectors are pinned into the 8 MB PSRAM pool instead of the ~298 KB
// internal heap (see word_pack.h PsramAllocator for the full rationale). A
// trivially copyable POD is realloc/move-safe inside the PSRAM vector; a
// std::string with a PSRAM allocator corrupts the heap during vector growth.
template <typename T>
struct NotePsramAllocator {
    using value_type = T;
    NotePsramAllocator() noexcept = default;
    template <typename U>
    NotePsramAllocator(const NotePsramAllocator<U>&) noexcept {}

    T* allocate(std::size_t n) {
        void* p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM);
        if (p == nullptr) {
            // Project builds with -fno-exceptions; abort rather than corrupt.
            abort();
        }
        return static_cast<T*>(p);
    }
    void deallocate(T* p, std::size_t) noexcept { heap_caps_free(p); }
};

template <typename A, typename B>
bool operator==(const NotePsramAllocator<A>&, const NotePsramAllocator<B>&) noexcept {
    return true;
}
template <typename A, typename B>
bool operator!=(const NotePsramAllocator<A>&, const NotePsramAllocator<B>&) noexcept {
    return false;
}

// A parsed note record (one JSONL line of a note pack). `content` carries the
// full plain_text_v1 body, so the device reads it on demand rather than keeping
// every body resident in the index.
struct WqnNoteEntry {
    std::string note_id;
    std::string notebook_id;
    std::string title;
    std::string content;
    // SHA-256 ids of the note's e-ink images (WQNI files) in display order,
    // straight from the pack line; empty for image-less notes.
    std::vector<std::string> image_ids;
    int32_t sort_index = 0;
    int revision = 0;
};

// Bytes of the note title cached inline for fast title-list rendering. Truncated
// on a UTF-8 boundary; the untruncated title comes from ReadNotePackEntry.
inline constexpr size_t kNoteListTitleBytes = 128;

// PSRAM-backed, fixed-size POD index entry. Deliberately NOT std::string for the
// heap-corruption reason documented in word_pack.h. file_offset points at the
// start of the note's JSONL line so a body read is a single seek.
struct NotePackIndexEntry {
    char note_id[37];
    char notebook_id[37];
    char pack_stem[28];
    char title[kNoteListTitleBytes];
    uint32_t file_offset;
    uint32_t notebook_order;
    int32_t sort_index;
    // Number of attached e-ink images (<= 4). The ids themselves stay in the
    // pack line and are materialised by ReadNotePackEntry on demand, keeping
    // the PSRAM index entry small.
    uint8_t image_count;
};

// A notebook row of the built index. There are few notebooks so std::string is
// fine here; entries[entry_begin .. entry_begin+entry_count) are this notebook's
// notes in sequential_note_v1 (sort_index, id) order.
struct NotePackNotebook {
    std::string notebook_id;
    std::string title;
    uint64_t content_revision = 0;
    uint64_t pack_revision = 0;
    std::string sha256;
    bool has_pack = false;
    size_t entry_begin = 0;
    size_t entry_count = 0;
};

// Compact identity used to confirm a session's pinned snapshot still matches the
// mounted content (consumed by the session store in a later milestone).
struct NotePackIdentity {
    char notebook_id[37] = {};
    uint64_t content_revision = 0;
    uint64_t pack_revision = 0;
    char sha256[65] = {};
};

struct NotePackIndex {
    bool mounted = false;
    bool has_manifest = false;
    bool pack_error = false;
    std::string status_message;
    size_t notebook_count = 0;
    size_t pack_bytes = 0;
    uint64_t manifest_revision = 0;
    std::vector<NotePackNotebook> notebooks;
    std::vector<NotePackIndexEntry, NotePsramAllocator<NotePackIndexEntry>> entries;
    // Indices into `entries` sorted by note_id (a globally-unique UUID) so a
    // note lookup is O(log N). `entries` itself stays in display order.
    std::vector<uint32_t, NotePsramAllocator<uint32_t>> note_order;
    std::vector<NotePackIdentity, NotePsramAllocator<NotePackIdentity>> pack_identities;
};

esp_err_t InitNotePackStorage();
esp_err_t ResetNotePackStorageCache();
esp_err_t LoadNotePackManifest(WqnNotePackManifest* manifest);
esp_err_t MergeNotePackManifestDelta(
    const WqnNotePackManifest& delta,
    WqnNotePackManifest* merged);
esp_err_t SaveNotePackManifest(const WqnNotePackManifest& manifest);
esp_err_t LoadNotePackIndex(NotePackIndex* index);
esp_err_t DownloadNotePackToStorage(
    const std::string& token,
    const protocol::v3::RequestMetadata& metadata,
    const WqnNotePackManifestNotebook& notebook);
bool NotePackNeedsDownload(const WqnNotePackManifestNotebook& notebook);
esp_err_t ReadNotePackEntry(const NotePackIndexEntry& index_entry, WqnNoteEntry* entry);
std::string SafeNotePackStem(const WqnNotePackManifestNotebook& notebook);

// --- Note image (WQNI) container + SPIFFS cache -----------------------------
// WQNI file: 20-byte little-endian header (magic "WQNI", version u8,
// pixel_format u8, flags u16, width u16, height u16, payload_length u32,
// crc32 u32) followed by the raw 1-bpp framebuffer payload. The payload uses
// the exact wqn_epd layout (row-major, 50 bytes/row, MSB-first, 1 = white),
// so display is a straight memcpy into the framebuffer.
inline constexpr size_t kNoteImageHeaderBytes = 20;
inline constexpr size_t kNoteImagePayloadBytes = 15000;  // 400x300 / 8
inline constexpr size_t kNoteImageFileBytes =
    kNoteImageHeaderBytes + kNoteImagePayloadBytes;
inline constexpr size_t kNoteImageCacheMaxFiles = 64;

// Validates magic/version/format/flags/geometry/length and the payload CRC32.
esp_err_t ValidateNoteImageWqni(const uint8_t* data, size_t size);
// Cache files live at /storage/ni_<image_id[0..12)>.wqni; the id is the
// SHA-256 of the whole file, so cached bytes are immutable.
esp_err_t LoadCachedNoteImage(
    const std::string& image_id, std::vector<uint8_t>* wqni);
esp_err_t StoreCachedNoteImage(
    const std::string& image_id, const uint8_t* data, size_t size);

}  // namespace wqn
