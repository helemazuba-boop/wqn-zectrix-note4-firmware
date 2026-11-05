// Host fixture for the REAL word_local_session.cpp pure module.
// The Python driver generates ESP/IDF stub headers, compiles the production
// translation unit main/word_local_session.cpp together with this file, and
// runs the checks below against the real production functions (no copied
// implementation).  It does NOT prove SPIFFS/NVS, power loss, or UI wiring.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "word_local_session.h"

static int passes = 0, failures = 0;
static void Check(bool ok, const char* name)
{
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    ok ? ++passes : ++failures;
}

using namespace wqn;
using protocol::word_study_v1::kMaxDecks;

// ---- deterministic 36-char UUID from an integer ---------------------------
static std::string Uuid(uint32_t n)
{
    char buf[37];
    std::snprintf(buf, sizeof(buf),
        "%08x-0000-4000-8000-%012x", n, n);
    return std::string(buf);
}

static WordPackIndex BuildIndex(uint32_t word_count, uint32_t deck_count,
                                uint64_t revision, char sha_fill,
                                bool shuffle)
{
    WordPackIndex index;
    index.mounted = true;
    index.has_manifest = true;
    index.truncated = false;
    index.pack_error = false;
    // One usable pin per deck.
    for (uint32_t d = 0; d < deck_count; ++d) {
        WordPackIdentity id;
        std::snprintf(id.deck_id, sizeof(id.deck_id), "%s",
                      Uuid(1000 + d).c_str());
        id.content_revision = revision;
        id.pack_revision = revision;
        std::memset(id.sha256, '0', 64);
        id.sha256[0] = sha_fill;
        id.sha256[63] = '\0';
        std::snprintf(id.sha256, sizeof(id.sha256),
                      "%s", std::string(64, sha_fill).c_str());
        index.pack_identities.push_back(id);
    }
    for (uint32_t i = 0; i < word_count; ++i) {
        WordPackIndexEntry e = {};
        std::snprintf(e.word_id, sizeof(e.word_id), "%s",
                      Uuid(50000 + i).c_str());
        uint32_t d = i % deck_count;
        std::snprintf(e.deck_id, sizeof(e.deck_id), "%s",
                      index.pack_identities[d].deck_id);
        std::snprintf(e.word, sizeof(e.word), "word_%06u", i);
        e.deck_order = d;
        e.sort_index = static_cast<int32_t>(i / deck_count);
        e.file_offset = i * 16;
        index.entries.push_back(e);
    }
    if (shuffle) {
        std::mt19937 rng(12345);
        std::shuffle(index.entries.begin(), index.entries.end(), rng);
    }
    return index;
}

// Walk the entire round using real production APIs only.
static void FullRound(uint32_t word_count, uint32_t deck_count, bool shuffle)
{
    char tag[160];
    std::snprintf(tag, sizeof(tag), "full round words=%u decks=%u shuffle=%d",
                  word_count, deck_count, shuffle ? 1 : 0);
    WordPackIndex index = BuildIndex(word_count, deck_count, 7, 'a', shuffle);

    std::vector<std::string> all_ids;
    std::set<std::string> seen;
    uint32_t start = 0;
    uint32_t generation = 3;
    bool duplicate = false, gap = false;
    uint64_t expected_absolute = 0;
    uint32_t segments = 0;

    while (true) {
        LocalWordSessionDescriptor descriptor;
        esp_err_t r = PrepareLocalWordSequence(
            index, "", start, Uuid(900000 + segments), "seed-A",
            generation, &descriptor);
        if (r == ESP_ERR_NOT_FOUND) break;  // full round complete
        if (r != ESP_OK) {
            std::snprintf(tag, sizeof(tag),
                "full round prepare failed at start=%u err=%d", start, r);
            Check(false, tag);
            return;
        }
        ++segments;
        Check(descriptor.total_count == word_count,
              "descriptor.total_count is whole-scope count");
        Check(descriptor.session.start_index == start,
              "descriptor.start_index is absolute segment start");
        Check(!descriptor.session.remote.items.empty() &&
              descriptor.session.remote.items.size() <= 500,
              "segment holds 1..500 full candidates");

        // Initial UI window (96) then roll it with real page + absolute cursor.
        PersistedWordSession window;
        r = BuildLocalWordInitialSession(descriptor, 96, &window);
        Check(r == ESP_OK, "BuildLocalWordInitialSession(96) succeeds");
        Check(window.remote.items.size() ==
                  std::min<size_t>(96, descriptor.session.remote.items.size()),
              "initial window capped at 96");

        const size_t segment_count = descriptor.session.remote.items.size();
        size_t delivered = window.remote.items.size();
        bool rolled = false;
        while (true) {
            // Visit every item currently in the 96 window, checking absolute
            // cursor continuity BEFORE advancing.
            while (window.position < window.remote.items.size()) {
                uint64_t absolute = 0;
                esp_err_t ar = WordSequenceAbsoluteCursor(window, &absolute);
                if (ar != ESP_OK) {
                    Check(false, "WordSequenceAbsoluteCursor valid in window");
                    return;
                }
                if (absolute != expected_absolute) gap = true;
                const auto& item = window.remote.items[window.position];
                if (!seen.insert(std::string(item.item_id)).second) {
                    duplicate = true;
                }
                all_ids.push_back(item.item_id);
                ++expected_absolute;
                ++window.position;  // answer the card
            }
            // At the window end, absolute cursor must be one-past the last.
            uint64_t absolute = 0;
            if (WordSequenceAbsoluteCursor(window, &absolute) == ESP_OK &&
                absolute != expected_absolute) {
                gap = true;
            }
            if (delivered >= segment_count) break;  // segment exhausted

            // Fetch the next local page and extend via the real production
            // extender (same one the online cloud runner uses).
            protocol::word_study_v1::CandidatePageData page;
            esp_err_t pr = GetLocalWordCandidatePage(
                descriptor, window.remote.cursor, 64, &page);
            if (pr == ESP_ERR_NOT_FOUND) break;
            if (pr != ESP_OK) {
                Check(false, "GetLocalWordCandidatePage succeeds mid-segment");
                return;
            }
            PersistedWordSession updated;
            esp_err_t er = ExtendPersistedWordSessionWithPage(
                window, page, &updated);
            if (er != ESP_OK) {
                std::snprintf(tag, sizeof(tag),
                    "Extend accepted local page (err=%d) seg=%u delivered=%zu",
                    er, segments, delivered);
                Check(false, tag);
                return;
            }
            rolled = true;
            delivered += page.items.size();
            window = std::move(updated);
            window.position = 0;  // prefix already answered/trimmed
        }
        // 96-window must have rolled for any segment longer than 96.
        if (segment_count > 96) {
            Check(rolled, "96 sliding window rolled via local pages");
        }
        start += static_cast<uint32_t>(segment_count);
    }

    Check(start == word_count, "all start segments sum to full scope count");
    Check(all_ids.size() == word_count,
          "every word observed exactly once (0 omission)");
    Check(!duplicate, "0 duplicate item_id across whole round");
    Check(!gap, "absolute cursor strictly continuous, no window roll-back");
    Check(expected_absolute == word_count,
          "absolute cursor ends at total_count");
}

int main()
{
    // 1) 3507 words, multiple decks, shuffled index -> deterministic full round.
    FullRound(3507, 5, true);
    // 2) Ordered index must produce the identical walk (determinism).
    FullRound(3507, 5, false);
    // 3) Boundary round: 500 + 499 + ... and 96/95 window edges.
    FullRound(1000, 3, true);
    FullRound(96, 1, false);
    FullRound(95, 1, false);
    FullRound(499, 2, true);
    FullRound(500, 2, true);

    // 4) Absolute cursor boundary values 95/96/499/500/3499/3507.
    {
        WordPackIndex index = BuildIndex(3507, 4, 1, 'a', true);
        LocalWordSessionDescriptor d0, d3499;
        Check(PrepareLocalWordSequence(index, "", 0, Uuid(1), "s", 1, &d0) == ESP_OK,
              "prepare segment @0");
        Check(PrepareLocalWordSequence(index, "", 3000, Uuid(2), "s", 1, &d3499) == ESP_OK,
              "prepare segment @3000");
        for (uint32_t boundary : {95u, 96u, 499u, 500u, 3499u, 3507u}) {
            // Build a synthetic session whose single item carries the boundary
            // as its ABSOLUTE scope ordinal, with start_index == 0.
            PersistedWordSession s;
            s.start_index = 0;
            s.position = 0;
            s.remote.items.resize(1);
            s.remote.items[0].ordinal = boundary;
            uint64_t out = 0;
            esp_err_t r = WordSequenceAbsoluteCursor(s, &out);
            char name[96];
            std::snprintf(name, sizeof(name),
                "absolute cursor boundary %u == itself", boundary);
            Check(r == ESP_OK && out == boundary, name);
        }
        // start_index added ONCE, not twice: item ordinal 5 at start 500 -> 505.
        PersistedWordSession s;
        s.start_index = 500;
        s.position = 0;
        s.remote.items.resize(1);
        s.remote.items[0].ordinal = 5;
        uint64_t out = 0;
        WordSequenceAbsoluteCursor(s, &out);
        Check(out == 505, "global cursor = start_index + item.ordinal (once)");
        // End-of-window one-past-last with a rolled window.
        s.remote.items.resize(2);
        s.remote.items[0].ordinal = 96;
        s.remote.items[1].ordinal = 97;
        s.position = 2;
        WordSequenceAbsoluteCursor(s, &out);
        Check(out == 598, "end cursor = start_index + last ordinal + 1");
        // Overflow / out-of-range guards.
        s.position = 3;
        Check(WordSequenceAbsoluteCursor(s, &out) == ESP_ERR_INVALID_STATE,
              "position past window rejected");
        s.start_index = UINT32_MAX;
        s.position = 0;
        s.remote.items.resize(1);
        s.remote.items[0].ordinal = 5;
        Check(WordSequenceAbsoluteCursor(s, &out) == ESP_ERR_INVALID_SIZE,
              "start_index + ordinal overflow rejected");
    }

    // 5) scope filtering: single deck selects only that deck's words.
    {
        WordPackIndex index = BuildIndex(600, 3, 1, 'a', true);
        std::string deck1 = Uuid(1001);
        LocalWordSessionDescriptor d;
        esp_err_t r = PrepareLocalWordSequence(index, deck1, 0, Uuid(3), "s", 1, &d);
        Check(r == ESP_OK, "single-deck scope prepares");
        Check(d.total_count == 200, "single-deck scope total == 200 of 600");
        Check(d.session.remote.deck_ids.size() == 1 &&
              std::strcmp(d.session.remote.deck_ids[0].value, deck1.c_str()) == 0,
              "scope deck set == the one deck");
        Check(d.session.remote.snapshot.size() == 1,
              "snapshot pin set == scope deck set");
        for (const auto& item : d.session.remote.items) {
            if (std::strcmp(item.deck_id, deck1.c_str()) != 0) {
                Check(false, "every scoped item belongs to the deck");
                break;
            }
        }
    }

    // 6) same-size but different sha => snapshot identity mismatch.
    {
        WordPackIndex base = BuildIndex(500, 2, 1, 'a', false);
        LocalWordSessionDescriptor d;
        Check(PrepareLocalWordSequence(base, "", 0, Uuid(4), "s", 1, &d) == ESP_OK,
              "prepare baseline descriptor");
        Check(LocalSequenceSnapshotMatches(base, d),
              "identical index matches pinned descriptor");
        WordPackIndex different_sha = BuildIndex(500, 2, 1, 'b', false);
        Check(different_sha.entries.size() == base.entries.size(),
              "same size different sha: size equal");
        Check(!LocalSequenceSnapshotMatches(different_sha, d),
              "same-size different-sha pack identity is a mismatch");
        WordPackIndex different_rev = BuildIndex(500, 2, 2, 'a', false);
        Check(!LocalSequenceSnapshotMatches(different_rev, d),
              "changed content_revision is a mismatch");
    }

    // 7) invalid index / over-32-deck / bad uuid / start == total rejections.
    {
        WordPackIndex bad = BuildIndex(100, 1, 1, 'a', false);
        LocalWordSessionDescriptor d;
        bad.truncated = true;
        Check(PrepareLocalWordSequence(bad, "", 0, Uuid(5), "s", 1, &d) ==
              ESP_ERR_INVALID_ARG, "truncated index rejected");
        bad = BuildIndex(100, 1, 1, 'a', false);
        bad.pack_error = true;
        Check(PrepareLocalWordSequence(bad, "", 0, Uuid(5), "s", 1, &d) ==
              ESP_ERR_INVALID_ARG, "pack_error index rejected");
        bad = BuildIndex(100, 1, 1, 'a', false);
        bad.pack_identities.clear();
        Check(PrepareLocalWordSequence(bad, "", 0, Uuid(5), "s", 1, &d) ==
              ESP_ERR_INVALID_ARG, "missing pin rejected");
        Check(PrepareLocalWordSequence(bad, "", 0, "not-a-uuid", "s", 1, &d) ==
              ESP_ERR_INVALID_ARG, "invalid session uuid rejected");
        // Path-injection UUID is rejected (contains '/').
        Check(PrepareLocalWordSequence(BuildIndex(10,1,1,'a',false), "", 0,
              "../etc/passwd0000000000000000000000000", "s", 1, &d) ==
              ESP_ERR_INVALID_ARG, "path-injection uuid rejected");
        // >32 decks: all-deck scope must fail INVALID_SIZE, not silently 32.
        WordPackIndex many = BuildIndex(3300, kMaxDecks + 1, 1, 'a', true);
        esp_err_t r = PrepareLocalWordSequence(many, "", 0, Uuid(6), "s", 1, &d);
        Check(r == ESP_ERR_INVALID_SIZE,
              "all-deck scope >32 rejected with INVALID_SIZE");
        // start == total -> NOT_FOUND (full complete, no wrap).
        WordPackIndex small = BuildIndex(40, 1, 1, 'a', false);
        Check(PrepareLocalWordSequence(small, "", 40, Uuid(7), "s", 1, &d) ==
              ESP_ERR_NOT_FOUND, "start_index == total -> full complete");
        Check(PrepareLocalWordSequence(small, "", 41, Uuid(7), "s", 1, &d) ==
              ESP_ERR_INVALID_RESPONSE, "start_index > total rejected");
    }

    // 8) page/cursor error paths + limit cap.
    {
        WordPackIndex index = BuildIndex(300, 1, 1, 'a', false);
        LocalWordSessionDescriptor d;
        PrepareLocalWordSequence(index, "", 0, Uuid(8), "s", 1, &d);
        protocol::word_study_v1::CandidatePageData page;
        Check(GetLocalWordCandidatePage(d, "0", 101, &page) == ESP_ERR_INVALID_ARG,
              "limit >100 rejected");
        Check(GetLocalWordCandidatePage(d, "abc", 10, &page) == ESP_ERR_INVALID_ARG,
              "non-numeric cursor rejected");
        Check(GetLocalWordCandidatePage(d, "500", 10, &page) == ESP_ERR_INVALID_STATE,
              "cursor past segment rejected");
        Check(GetLocalWordCandidatePage(d, "300", 10, &page) == ESP_ERR_NOT_FOUND,
              "cursor == segment end -> no more");
        Check(GetLocalWordCandidatePage(d, "0", 0, &page) == ESP_ERR_INVALID_ARG,
              "zero limit rejected");
    }

    // Fixed buffers are validated before any unbounded string comparison.
    {
        auto index=BuildIndex(40,1,1,'a',false);
        LocalWordSessionDescriptor valid,invalid;
        PrepareLocalWordSequence(index,"",0,Uuid(99),"s",1,&valid);
        auto bad_index=index;
        std::memset(bad_index.pack_identities[0].deck_id,'a',37);
        Check(PrepareLocalWordSequence(bad_index,"",0,Uuid(100),"s",1,&invalid)!=ESP_OK,
              "unterminated pack deck ID is rejected before comparison");
        Check(!LocalSequenceSnapshotMatches(bad_index,valid),"unterminated pin cannot match a snapshot");
        wqn::PersistedWordSession initial;
        invalid=valid;std::memset(invalid.session.remote.deck_ids[0].value,'a',37);
        Check(BuildLocalWordInitialSession(invalid,32,&initial)!=ESP_OK,"unterminated descriptor deck ID rejected");
        invalid=valid;std::memset(invalid.session.remote.items[0].item_id,'a',37);
        Check(BuildLocalWordInitialSession(invalid,32,&initial)!=ESP_OK,"unterminated descriptor item ID rejected");
        invalid=valid;
        std::snprintf(invalid.session.remote.items[0].deck_id,37,"%s",Uuid(999).c_str());
        Check(BuildLocalWordInitialSession(invalid,32,&initial)!=ESP_OK,"candidate outside descriptor scope rejected");
    }

    std::printf("%d PASS / %d FAIL (real word_local_session.cpp; NOT HIL)\n",
                passes, failures);
    return failures ? 1 : 0;
}
