#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "esp_err.h"
#include "device_protocol/problem_study.h"
// Problem packs reuse the note pack's PSRAM allocator and the shared ni_
// WQNI image cache (problem images are content-addressed WQNI files too).
#include "note_pack.h"
#include "wqn_api.h"

namespace wqn {

// One typed sub-question of a problem shell, straight from the pack line.
// answer_text is display-ready (choice letters already joined for MCQ); the
// device never parses answer_config.
struct WqnProblemPackPart {
    int index = 0;
    std::string label;
    std::string type;
    int full_marks = 0;
    std::string content_text;
    std::string answer_text;
};

// A parsed problem record (one JSONL line of a problem pack). Bodies and
// parts are read on demand via ReadProblemPackEntry rather than kept
// resident in the index.
struct WqnProblemEntry {
    std::string problem_id;
    std::string title;
    std::string content_text;
    std::vector<WqnProblemPackPart> parts;
    protocol::problem_study_v1::ProblemStatus status =
        protocol::problem_study_v1::ProblemStatus::kWrong;
    bool is_optional = false;
    // SHA-256 ids of the problem/solution e-ink images (WQNI files) in
    // display order; empty when the problem has no attachments.
    std::vector<std::string> image_ids;
    std::vector<std::string> gray4_image_ids;
    std::vector<std::string> solution_image_ids;
    std::vector<std::string> solution_gray4_image_ids;
};

// Bytes of the problem title cached inline for fast title-list rendering.
// Truncated on a UTF-8 boundary; the full title comes from ReadProblemPackEntry.
inline constexpr size_t kProblemListTitleBytes = 128;

// PSRAM-backed, fixed-size POD index entry (see note_pack.h for the
// no-std::string rationale). file_offset points at the start of the
// problem's JSONL line so a body read is a single seek.
struct ProblemPackIndexEntry {
    char problem_id[37];
    char set_id[37];
    char pack_stem[28];
    char title[kProblemListTitleBytes];
    uint32_t file_offset;
    uint32_t set_order;
    // Attachment counts (<= 8 each); the ids themselves stay in the pack
    // line and are materialised by ReadProblemPackEntry on demand.
    uint8_t image_count;
    uint8_t solution_image_count;
    // protocol::problem_study_v1::ProblemStatus as a raw value.
    uint8_t status;
};

// A problem-set row of the built index. entries[entry_begin ..
// entry_begin+entry_count) are this set's problems in fixed pack order.
struct ProblemPackSet {
    std::string set_id;
    std::string name;
    bool is_smart = false;
    uint64_t pack_revision = 0;
    std::string sha256;
    bool has_pack = false;
    size_t entry_begin = 0;
    size_t entry_count = 0;
};

struct ProblemPackIndex {
    bool mounted = false;
    bool has_manifest = false;
    bool pack_error = false;
    std::string status_message;
    size_t set_count = 0;
    size_t pack_bytes = 0;
    std::vector<ProblemPackSet> sets;
    std::vector<ProblemPackIndexEntry, NotePsramAllocator<ProblemPackIndexEntry>> entries;
    // Indices into `entries` sorted by problem_id so a lookup is O(log N);
    // `entries` itself stays in display (pack) order.
    std::vector<uint32_t, NotePsramAllocator<uint32_t>> problem_order;
};

esp_err_t InitProblemPackStorage();
esp_err_t ResetProblemPackStorageCache();
esp_err_t LoadProblemPackManifest(WqnProblemPackManifest* manifest);
esp_err_t MergeProblemPackManifestDelta(
    const WqnProblemPackManifest& delta,
    WqnProblemPackManifest* merged);
esp_err_t SaveProblemPackManifest(const WqnProblemPackManifest& manifest);
esp_err_t LoadProblemPackIndex(ProblemPackIndex* index);
esp_err_t DownloadProblemPackToStorage(
    const std::string& token,
    const protocol::v3::RequestMetadata& metadata,
    const WqnProblemPackManifestSet& set);
// verified_sha: sha recorded in the local manifest when this pack was last
// downloaded (already verified end to end); when it matches the listing and
// the file size is intact the full-file re-hash is skipped (relist-every-sync
// cadence, see note_pack.h).
bool ProblemPackNeedsDownload(
    const WqnProblemPackManifestSet& set,
    const std::string* verified_sha = nullptr);
esp_err_t ReadProblemPackEntry(
    const ProblemPackIndexEntry& index_entry, WqnProblemEntry* entry);
// Parses one pack JSONL record. Exposed for the contract fixture self-test;
// include_content=false skips materialising bodies/parts during index scans.
esp_err_t ParseProblemRecordLine(
    const char* line, WqnProblemEntry* entry, bool include_content);
std::string SafeProblemPackStem(const WqnProblemPackManifestSet& set);

}  // namespace wqn
