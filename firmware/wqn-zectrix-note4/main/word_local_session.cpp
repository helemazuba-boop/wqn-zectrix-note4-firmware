#include "word_local_session.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
#include "mbedtls/sha256.h"

namespace wqn {
namespace {

using protocol::word_study_v1::kMaxDecks;
using protocol::word_study_v1::kMaxSessionItems;
using protocol::word_study_v1::kMaxCandidatePageItems;
using protocol::word_study_v1::Mode;
using protocol::word_study_v1::Ordering;
using protocol::word_study_v1::Purpose;

inline constexpr uint32_t kMaxInitialWindow = 96;
inline constexpr uint32_t kDefaultInitialWindow = 32;
inline constexpr size_t kMaxWordText = 80;   // WordPackIndexEntry::word[81]
inline constexpr uint64_t kMaxSafeRevision =
    wqn::protocol::v3::kMaxSafeJsonInteger;

// PSRAM-backed scratch.  The full-scope ordering vector holds one POD pointer
// per word (<=10000, explicitly SPIRAM); no per-word std::string is ever built,
// so this does not reintroduce the Note4 internal-heap small-allocation shape.
template <typename T>
using ScratchVector = std::vector<T, WordStorePsramAllocator<T>>;

using StoredSnapshotVector = std::vector<
    StoredWordPackSnapshot,
    WordStorePsramAllocator<StoredWordPackSnapshot>>;

// Bounded fixed-buffer helpers: never walk past a POD array looking for NUL.
bool FixedTextEquals(const char* buffer, size_t capacity, const std::string& value)
{
    if (value.size() >= capacity) return false;
    for (size_t i = 0; i < value.size(); ++i) {
        if (buffer[i] != value[i]) return false;
    }
    return buffer[value.size()] == '\0';
}

bool FixedTextLength(const char* buffer, size_t capacity, size_t expected)
{
    if (expected >= capacity) return false;
    for (size_t i = 0; i < expected; ++i) {
        if (buffer[i] == '\0') return false;
    }
    return buffer[expected] == '\0';
}

bool IsLowerHex(const char* buffer, size_t capacity, size_t expected)
{
    if (!FixedTextLength(buffer, capacity, expected)) return false;
    for (size_t i = 0; i < expected; ++i) {
        const char ch = buffer[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool IsUrlSafeSeed(const std::string& value)
{
    // Server policy: seed is 1..64 url-safe chars; EMPTY is rejected.
    if (value.empty() || value.size() > 64) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
    });
}

// Strict pin identity: complete UUID, lowercase sha256 hex, positive bounded
// pack_revision, bounded content_revision (no NUL-length-only checks).
bool ValidPin(const WordPackIdentity& identity)
{
    if (!FixedTextLength(identity.deck_id, sizeof(identity.deck_id), 36)) return false;
    if (!IsValidLocalSessionUuid(std::string(identity.deck_id))) return false;
    if (!IsLowerHex(identity.sha256, sizeof(identity.sha256), 64)) return false;
    if (identity.pack_revision == 0 || identity.pack_revision > kMaxSafeRevision ||
        identity.content_revision > kMaxSafeRevision) {
        return false;
    }
    return true;
}

// ASCII lower-case compare of two BOUNDED POD word fields, computed on the fly
// (no per-word std::string allocation).  Returns <0 / 0 / >0.
int CompareNormalizedWord(const char* a, const char* b)
{
    for (size_t i = 0; i <= kMaxWordText; ++i) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        if (ca == 0 && cb == 0) return 0;
        if (ca == 0) return -1;
        if (cb == 0) return 1;
        ca = static_cast<unsigned char>(std::tolower(ca));
        cb = static_cast<unsigned char>(std::tolower(cb));
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    return 0;  // both bounded fields exhausted identically
}

struct OrderedWord {
    const WordPackIndexEntry* entry;
};

// Mirrors the online DeckOrderLess comparator: (deck_order, sort_index,
// normalized word, word_id).  Used both for the initial sort and for deriving
// the canonical traversal order, so the two can never disagree.
bool LocalCandidateLess(const OrderedWord& a, const OrderedWord& b)
{
    const WordPackIndexEntry& ea = *a.entry;
    const WordPackIndexEntry& eb = *b.entry;
    if (ea.deck_order != eb.deck_order) return ea.deck_order < eb.deck_order;
    if (ea.sort_index != eb.sort_index) return ea.sort_index < eb.sort_index;
    const int word_cmp = CompareNormalizedWord(ea.word, eb.word);
    if (word_cmp != 0) return word_cmp < 0;
    return std::strcmp(ea.word_id, eb.word_id) < 0;
}

// Derive the canonical cross-deck traversal order from a sorted candidate list:
// the sequence of distinct deck_ids in first-appearance order, plus each deck's
// word count.  This is what gets pinned into deck_ids/snapshot and re-checked by
// LocalSequenceSnapshotMatches; it is independent of raw deck_order numbers so
// a pinned rebuild that renumbers deck_order can still be validated.
struct TraversalDeck {
    char deck_id[37];
    uint32_t first_deck_order;
    int32_t min_sort_index;
    size_t count;
};

esp_err_t DeriveTraversalOrder(
    const ScratchVector<OrderedWord>& ordered,
    size_t begin,
    size_t end,
    ScratchVector<TraversalDeck>* decks)
{
    decks->clear();
    for (size_t i = begin; i < end; ++i) {
        const WordPackIndexEntry& entry = *ordered[i].entry;
        TraversalDeck* found = nullptr;
        for (TraversalDeck& deck : *decks) {
            if (std::strcmp(deck.deck_id, entry.deck_id) == 0) {
                found = &deck;
                break;
            }
        }
        if (found == nullptr) {
            if (decks->size() >= kMaxDecks) return ESP_ERR_INVALID_SIZE;
            TraversalDeck deck = {};
            std::memcpy(deck.deck_id, entry.deck_id, sizeof(deck.deck_id));
            deck.first_deck_order = entry.deck_order;
            deck.min_sort_index = entry.sort_index;
            deck.count = 1;
            decks->push_back(deck);
        } else {
            ++found->count;
            if (entry.sort_index < found->min_sort_index) {
                found->min_sort_index = entry.sort_index;
            }
        }
    }
    return ESP_OK;
}

// Build the selected scope's pinned decks/snapshot IN canonical traversal order.
// `deck_id` empty -> all decks with a usable pin; else that single deck.
esp_err_t ResolveScope(
    const WordPackIndex& index,
    const std::string& deck_id,
    const ScratchVector<TraversalDeck>& traversal,
    StoredSnapshotVector* snapshot,
    std::vector<StoredWordDeckId,
        WordStorePsramAllocator<StoredWordDeckId>>* deck_ids)
{
    snapshot->clear();
    deck_ids->clear();
    if (!deck_id.empty() && !IsValidLocalSessionUuid(deck_id)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (traversal.empty()) return ESP_ERR_INVALID_SIZE;
    if (traversal.size() > kMaxDecks) return ESP_ERR_INVALID_SIZE;  // no silent 32-trim

    for (const TraversalDeck& deck : traversal) {
        if (!deck_id.empty() &&
            !FixedTextEquals(deck.deck_id, sizeof(deck.deck_id), deck_id)) {
            continue;  // single-deck scope: skip other traversal decks
        }
        const auto pin = std::find_if(
            index.pack_identities.begin(), index.pack_identities.end(),
            [&](const WordPackIdentity& value) {
                return FixedTextLength(value.deck_id, sizeof(value.deck_id), 36) &&
                    std::memcmp(value.deck_id, deck.deck_id, 36) == 0;
            });
        if (pin == index.pack_identities.end() || !ValidPin(*pin)) {
            return ESP_ERR_INVALID_ARG;  // missing / malformed pin
        }
        StoredWordPackSnapshot stored;
        std::memcpy(stored.deck_id, pin->deck_id, sizeof(stored.deck_id));
        stored.content_revision = pin->content_revision;
        stored.pack_revision = pin->pack_revision;
        std::memcpy(stored.sha256, pin->sha256, sizeof(stored.sha256));
        snapshot->push_back(stored);
        StoredWordDeckId id;
        std::memcpy(id.value, pin->deck_id, sizeof(id.value));
        deck_ids->push_back(id);
    }
    if (snapshot->empty()) return ESP_ERR_INVALID_SIZE;  // empty scope
    return ESP_OK;
}

bool ScopeContains(
    const std::vector<StoredWordDeckId,
        WordStorePsramAllocator<StoredWordDeckId>>& deck_ids,
    const char* deck_id)
{
    // Bounded linear find: deck_ids is kept in traversal order, NOT sorted, so
    // binary_search would be wrong.  <=32 entries -> linear is fine.
    for (const StoredWordDeckId& value : deck_ids) {
        if (std::strcmp(value.value, deck_id) == 0) return true;
    }
    return false;
}

// Parse the local cursor (decimal "next local ordinal").  Empty -> 0.
bool ParseLocalCursor(const std::string& cursor, uint32_t* offset)
{
    if (offset == nullptr) return false;
    if (cursor.empty()) {
        *offset = 0;
        return true;
    }
    if (cursor.size() > 10) return false;
    uint64_t value = 0;
    for (char ch : cursor) {
        if (ch < '0' || ch > '9') return false;
        value = value * 10U + static_cast<uint64_t>(ch - '0');
        if (value > std::numeric_limits<uint32_t>::max()) return false;
    }
    *offset = static_cast<uint32_t>(value);
    return true;
}

// Validate full descriptor semantics (shared by Load path and tests):
// total_count / 500 candidates / dense ordinals / unique item ids / traversal
// order consistent deck_ids+snapshot / strict pin identity / scope pin matches.
bool DescriptorSemanticsValid(const LocalWordSessionDescriptor& descriptor)
{
    const PersistedWordSession& s = descriptor.session;
    const StoredWordSessionData& r = s.remote;
    if (!s.active || s.paused || s.position != 0 ||
        s.phase != WordPresentationPhase::kFront ||
        !((r.mode == Mode::kSequential && r.ordering == Ordering::kSequential && r.include_mastered) ||
          (r.mode == Mode::kShuffle && r.ordering == Ordering::kPureRandomV1 && !r.include_mastered)) || r.purpose != Purpose::kStudy ||
        !IsUrlSafeSeed(r.seed) || r.next_sequence != 0 ||
        r.progress_revision > kMaxSafeRevision || r.has_more || !r.cursor.empty() ||
        r.items.empty() ||
        r.items.size() > kMaxSessionItems) {
        return false;
    }
    if (!IsValidLocalSessionUuid(r.session_id)) return false;
    if (descriptor.total_count > 10000 || descriptor.total_count <= s.start_index ||
        r.items.size() > descriptor.total_count - s.start_index) {
        return false;
    }
    if (r.deck_ids.empty() || r.deck_ids.size() > kMaxDecks ||
        r.deck_ids.size() != r.snapshot.size()) {
        return false;
    }
    for (size_t i = 0; i < r.deck_ids.size(); ++i) {
        if (!FixedTextLength(r.deck_ids[i].value, sizeof(r.deck_ids[i].value), 36) ||
            !FixedTextLength(r.snapshot[i].deck_id, sizeof(r.snapshot[i].deck_id), 36) ||
            !IsValidLocalSessionUuid(std::string(r.deck_ids[i].value)) ||
            std::strcmp(r.deck_ids[i].value, r.snapshot[i].deck_id) != 0) {
            return false;
        }
        for (size_t j = 0; j < i; ++j) {
            if (std::strcmp(r.deck_ids[i].value, r.deck_ids[j].value) == 0) return false;
        }
        const StoredWordPackSnapshot& snap = r.snapshot[i];
        if (!IsLowerHex(snap.sha256, sizeof(snap.sha256), 64) ||
            snap.pack_revision == 0 || snap.pack_revision > kMaxSafeRevision ||
            snap.content_revision > kMaxSafeRevision) {
            return false;
        }
    }
    // Dense segment-local ordinals + globally unique item ids (within a deck
    // AND across decks).  A duplicate would shift global offsets, so reject.
    for (size_t i = 0; i < r.items.size(); ++i) {
        if (r.items[i].ordinal != i) return false;
        if (!FixedTextLength(r.items[i].item_id, sizeof(r.items[i].item_id), 36) ||
            !FixedTextLength(r.items[i].deck_id, sizeof(r.items[i].deck_id), 36) ||
            !IsValidLocalSessionUuid(std::string(r.items[i].item_id)) ||
            !IsValidLocalSessionUuid(std::string(r.items[i].deck_id))) {
            return false;
        }
        if (std::none_of(r.deck_ids.begin(), r.deck_ids.end(), [&](const StoredWordDeckId& id) {
                return std::strcmp(id.value, r.items[i].deck_id) == 0;
            })) return false;
        for (size_t j = 0; j < i; ++j) {
            if (std::strcmp(r.items[i].item_id, r.items[j].item_id) == 0) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace


std::string WordLocalSha256(const std::string& bytes)
{
    unsigned char digest[32] = {};
    if (mbedtls_sha256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), digest,
                       0) != 0)
        return {};
    char hex[65] = {};
    for (size_t i = 0; i < 32; ++i)
        std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return hex;
}

bool LocalWordEligibilityMatches(const WordPackIndex& index,
                                 const LocalWordEligibility& eligibility, const std::string& scope)
{
    if (!index.mounted || !index.has_manifest || index.truncated || index.pack_error ||
        !IsLowerHex(eligibility.actor_binding.c_str(), eligibility.actor_binding.size() + 1, 64) ||
        !IsLowerHex(eligibility.item_ids_sha256.c_str(), eligibility.item_ids_sha256.size() + 1,
                    64) ||
        eligibility.progress_revision > kMaxSafeRevision ||
        (!eligibility.scope_deck_id.empty() && eligibility.scope_deck_id != scope) ||
        eligibility.snapshot.empty() || eligibility.snapshot.size() > kMaxDecks)
        return false;
    for (size_t i = 0; i < eligibility.snapshot.size(); ++i)
    {
        const auto& saved = eligibility.snapshot[i];
        if (!FixedTextLength(saved.deck_id, sizeof(saved.deck_id), 36) ||
            !IsLowerHex(saved.sha256, sizeof(saved.sha256), 64) ||
            !IsValidLocalSessionUuid(saved.deck_id))
            return false;
        for (size_t j = 0; j < i; ++j)
            if (std::strcmp(saved.deck_id, eligibility.snapshot[j].deck_id) == 0)
                return false;
        auto at = std::find_if(index.pack_identities.begin(), index.pack_identities.end(),
                               [&](const auto& pin)
                               {
                                   return ValidPin(pin) &&
                                          std::strcmp(saved.deck_id, pin.deck_id) == 0 &&
                                          std::strcmp(saved.sha256, pin.sha256) == 0 &&
                                          saved.pack_revision == pin.pack_revision &&
                                          saved.content_revision == pin.content_revision;
                               });
        if (at == index.pack_identities.end())
            return false;
    }
    ScratchVector<const WordPackIndexEntry*> ids;
    for (const auto& e : index.entries)
    {
        if (!FixedTextLength(e.word_id, sizeof(e.word_id), 36) ||
            !FixedTextLength(e.deck_id, sizeof(e.deck_id), 36) ||
            !IsValidLocalSessionUuid(e.word_id) || !IsValidLocalSessionUuid(e.deck_id))
            return false;
        if (eligibility.scope_deck_id.empty() || eligibility.scope_deck_id == e.deck_id)
        {
            if (std::none_of(eligibility.snapshot.begin(), eligibility.snapshot.end(),
                             [&](const auto& pin)
                             { return std::strcmp(pin.deck_id, e.deck_id) == 0; }))
                return false;
            ids.push_back(&e);
        }
    }
    if (ids.empty() || ids.size() > 10000 || ids.size() != eligibility.mastered_bitmap.size())
        return false;
    std::sort(ids.begin(), ids.end(),
              [](const auto* a, const auto* b) { return std::strcmp(a->word_id, b->word_id) < 0; });
    std::string canonical;
    canonical.reserve(ids.size() * 37);
    for (size_t i = 0; i < ids.size(); ++i)
    {
        if (i > 0)
        {
            if (std::strcmp(ids[i - 1]->word_id, ids[i]->word_id) == 0)
                return false;
            canonical += '\n';
        }
        canonical += ids[i]->word_id;
        if (eligibility.mastered_bitmap[i] != '0' && eligibility.mastered_bitmap[i] != '1')
            return false;
    }
    return WordLocalSha256(canonical) == eligibility.item_ids_sha256;
}

bool IsValidLocalSessionUuid(const std::string& value)
{
    if (value.size() != 36) return false;
    for (size_t index = 0; index < value.size(); ++index) {
        const char ch = value[index];
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (ch != '-') return false;
        } else if (!((ch >= '0' && ch <= '9') ||
                     (ch >= 'a' && ch <= 'f') ||
                     (ch >= 'A' && ch <= 'F'))) {
            return false;
        }
    }
    return true;
}

esp_err_t PrepareLocalWordSequence(const WordPackIndex& index, const std::string& deck_id,
                                   uint32_t start_index, const std::string& session_uuid,
                                   const std::string& seed, uint32_t generation,
                                   LocalWordSessionDescriptor* out, Mode mode,
                                   const LocalWordEligibility* eligibility)
{
    if (mode != Mode::kSequential && mode != Mode::kShuffle)
        return ESP_ERR_INVALID_ARG;
    if (mode == Mode::kShuffle &&
        (eligibility == nullptr || !LocalWordEligibilityMatches(index, *eligibility, deck_id)))
        return ESP_ERR_INVALID_STATE;
    if (out == nullptr)
        return ESP_ERR_INVALID_ARG;
    *out = {};
    if (!index.mounted || !index.has_manifest || index.truncated || index.pack_error ||
        index.entries.empty() || index.pack_identities.empty())
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!IsValidLocalSessionUuid(session_uuid))
        return ESP_ERR_INVALID_ARG;
    if (!IsUrlSafeSeed(seed))
        return ESP_ERR_INVALID_ARG; // empty seed rejected
    if (!deck_id.empty() && !IsValidLocalSessionUuid(deck_id))
    {
        return ESP_ERR_INVALID_ARG;
    }
    // Every entry id must be a well-formed UUID (bounded, no strlen overflow).
    for (const WordPackIndexEntry& entry : index.entries)
    {
        if (!FixedTextLength(entry.word_id, sizeof(entry.word_id), 36) ||
            !FixedTextLength(entry.deck_id, sizeof(entry.deck_id), 36) ||
            !IsValidLocalSessionUuid(std::string(entry.word_id)) ||
            !IsValidLocalSessionUuid(std::string(entry.deck_id)))
        {
            return ESP_ERR_INVALID_ARG;
        }
    }

    // Apply the requested deck before deriving the traversal: the 32-deck
    // session limit bounds the selected scope, not every cached manifest.
    // PSRAM scratch, POD pointers, no per-word std::string.
    ScratchVector<OrderedWord> ordered;
    ordered.reserve(index.entries.size());
    for (const WordPackIndexEntry& entry : index.entries)
    {
        if (!deck_id.empty() && !FixedTextEquals(entry.deck_id, sizeof(entry.deck_id), deck_id))
        {
            continue;
        }
        ordered.push_back({&entry});
    }
    std::sort(ordered.begin(), ordered.end(), LocalCandidateLess);
    if (ordered.size() > std::numeric_limits<uint32_t>::max())
    {
        return ESP_ERR_INVALID_SIZE;
    }

    // Canonical cross-deck traversal order derived from the SAME comparator.
    ScratchVector<TraversalDeck> traversal;
    esp_err_t order_result = DeriveTraversalOrder(ordered, 0, ordered.size(), &traversal);
    if (order_result != ESP_OK)
        return order_result;

    StoredSnapshotVector snapshot;
    std::vector<StoredWordDeckId, WordStorePsramAllocator<StoredWordDeckId>> deck_ids;
    esp_err_t scope_result = ResolveScope(index, deck_id, traversal, &snapshot, &deck_ids);
    if (scope_result != ESP_OK)
        return scope_result;

    // Restrict the ordered list to the selected scope and re-sort (the
    // comparator is deterministic, so this equals the original relative order).
    ScratchVector<OrderedWord> scoped;
    scoped.reserve(ordered.size());
    for (const OrderedWord& ow : ordered)
    {
        if (ScopeContains(deck_ids, ow.entry->deck_id))
            scoped.push_back(ow);
    }
    if (mode == Mode::kShuffle)
    {
        ScratchVector<const WordPackIndexEntry*> by_id;
        for (const auto& e : index.entries)
        {
            if (eligibility->scope_deck_id.empty() || eligibility->scope_deck_id == e.deck_id)
                by_id.push_back(&e);
        }
        std::sort(by_id.begin(), by_id.end(), [](const auto* a, const auto* b)
                  { return std::strcmp(a->word_id, b->word_id) < 0; });
        scoped.erase(
            std::remove_if(
                scoped.begin(), scoped.end(),
                [&](const OrderedWord& word)
                {
                    auto at = std::lower_bound(by_id.begin(), by_id.end(), word.entry,
                                               [](const auto* a, const auto* b)
                                               { return std::strcmp(a->word_id, b->word_id) < 0; });
                    return at == by_id.end() ||
                           std::strcmp((*at)->word_id, word.entry->word_id) != 0 ||
                           eligibility->mastered_bitmap[static_cast<size_t>(at - by_id.begin())] ==
                               '1';
                }),
            scoped.end());
        if (scoped.empty())
            return ESP_ERR_NOT_FOUND;
    }
    if (scoped.empty())
        return ESP_ERR_INVALID_SIZE;
    std::sort(scoped.begin(), scoped.end(), LocalCandidateLess);

    // Duplicate item ids anywhere in the scope must fail, not silently shift
    // global offsets across the 500 boundary.
    // Reuse the already allocated ordering scratch for a UUID-only sort.
    // Keep scoped in traversal order; 10k words must not require 50M strcmp
    // calls just to prove uniqueness before preparing the first segment.
    ordered.assign(scoped.begin(), scoped.end());
    std::sort(ordered.begin(), ordered.end(), [](const OrderedWord& left, const OrderedWord& right)
              { return std::strcmp(left.entry->word_id, right.entry->word_id) < 0; });
    for (size_t i = 1; i < ordered.size(); ++i)
    {
        if (std::strcmp(ordered[i - 1].entry->word_id, ordered[i].entry->word_id) == 0)
        {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }

    if (mode == Mode::kShuffle)
    {
        const auto rank = [&](const char* id)
        {
            uint64_t hash = UINT64_C(0xcbf29ce484222325);
            for (unsigned char c : seed)
            {
                hash ^= c;
                hash *= UINT64_C(0x100000001b3);
            }
            hash ^= 0;
            hash *= UINT64_C(0x100000001b3);
            for (const unsigned char* c = reinterpret_cast<const unsigned char*>(id); *c; ++c)
            {
                hash ^= *c;
                hash *= UINT64_C(0x100000001b3);
            }
            return hash;
        };
        std::sort(scoped.begin(), scoped.end(),
                  [&](const OrderedWord& a, const OrderedWord& b)
                  {
                      const auto ah = rank(a.entry->word_id), bh = rank(b.entry->word_id);
                      return ah != bh ? ah < bh
                                      : std::strcmp(a.entry->word_id, b.entry->word_id) < 0;
                  });
    }
    const uint32_t total = static_cast<uint32_t>(scoped.size());
    if (start_index > total)
        return ESP_ERR_INVALID_RESPONSE;
    if (start_index == total)
        return ESP_ERR_NOT_FOUND; // full complete, no wrap
    const uint32_t segment_count =
        static_cast<uint32_t>(std::min<size_t>(kMaxSessionItems, total - start_index));

    LocalWordSessionDescriptor descriptor;
    descriptor.total_count = total;
    PersistedWordSession& session = descriptor.session;
    session.active = true;
    session.paused = false;
    session.position = 0;
    session.start_index = start_index;
    session.phase = WordPresentationPhase::kFront;
    session.deck_scope_generation = generation;

    StoredWordSessionData& remote = session.remote;
    remote.session_id = session_uuid;
    remote.mode = mode;
    remote.purpose = Purpose::kStudy;
    remote.ordering = mode == Mode::kShuffle ? Ordering::kPureRandomV1 : Ordering::kSequential;
    remote.seed = seed;
    remote.include_mastered = mode == Mode::kSequential;
    remote.optional_count = static_cast<int>(kMaxSessionItems);
    remote.next_sequence = 0;
    remote.progress_revision = eligibility == nullptr ? 0 : eligibility->progress_revision;
    remote.has_more = false;
    remote.cursor.clear();
    remote.deck_ids = std::move(deck_ids); // traversal order
    remote.snapshot = std::move(snapshot); // same traversal order
    remote.items.reserve(segment_count);
    // Segment-local DENSE ordinals 0..segment_count-1.  start_index is added
    // ONLY by WordSequenceAbsoluteCursor, never here (no double add).
    for (uint32_t i = 0; i < segment_count; ++i)
    {
        const WordPackIndexEntry& entry = *scoped[start_index + i].entry;
        StoredWordSessionItem item;
        std::memcpy(item.item_id, entry.word_id, sizeof(item.item_id));
        std::memcpy(item.deck_id, entry.deck_id, sizeof(item.deck_id));
        item.ordinal = i;
        remote.items.push_back(item);
    }
    if (!DescriptorSemanticsValid(descriptor))
        return ESP_ERR_INVALID_RESPONSE;
    *out = std::move(descriptor);
    return ESP_OK;
}

esp_err_t BuildLocalWordInitialSession(
    const LocalWordSessionDescriptor& descriptor,
    uint32_t max_initial,
    PersistedWordSession* out)
{
    if (out == nullptr) return ESP_ERR_INVALID_ARG;
    *out = {};
    if (max_initial == 0) max_initial = kDefaultInitialWindow;
    if (max_initial > kMaxInitialWindow) return ESP_ERR_INVALID_SIZE;
    if (!DescriptorSemanticsValid(descriptor)) return ESP_ERR_INVALID_STATE;

    const size_t segment_count = descriptor.session.remote.items.size();
    const size_t kept = std::min<size_t>(max_initial, segment_count);
    PersistedWordSession session = descriptor.session;
    session.remote.items.resize(kept);
    session.position = 0;
    session.remote.cursor = std::to_string(static_cast<unsigned>(kept));
    // has_more = remaining candidates WITHIN this <=500 segment only.
    session.remote.has_more = kept < segment_count;
    *out = std::move(session);
    return ESP_OK;
}

esp_err_t GetLocalWordCandidatePage(
    const LocalWordSessionDescriptor& descriptor,
    const std::string& cursor,
    uint32_t limit,
    protocol::word_study_v1::CandidatePageData* out)
{
    if (out == nullptr || limit == 0 || limit > kMaxCandidatePageItems) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = {};
    const PersistedWordSession& session = descriptor.session;
    const auto& items = session.remote.items;
    if (!session.active || items.empty() || items.size() > kMaxSessionItems) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t offset = 0;
    if (!ParseLocalCursor(cursor, &offset)) return ESP_ERR_INVALID_ARG;
    if (offset > items.size()) return ESP_ERR_INVALID_STATE;
    if (offset == items.size()) return ESP_ERR_NOT_FOUND;  // segment exhausted

    const size_t count = std::min<size_t>(limit, items.size() - offset);
    protocol::word_study_v1::CandidatePageData page;
    page.session_id = session.remote.session_id;
    page.ordering = session.remote.ordering;
    page.candidate_policy_version =
        protocol::word_study_v1::CandidatePolicyVersionName(
            session.remote.ordering);
    page.seed = session.remote.seed;
    page.progress_revision = session.remote.progress_revision;
    page.cursor = cursor;
    page.next_cursor = std::to_string(
        static_cast<unsigned>(offset + count));
    page.has_more = (offset + count) < items.size();
    page.snapshot.reserve(session.remote.snapshot.size());
    for (const StoredWordPackSnapshot& stored : session.remote.snapshot) {
        protocol::word_study_v1::PackSnapshot pinned;
        pinned.deck_id = stored.deck_id;
        pinned.content_revision = stored.content_revision;
        pinned.pack_revision = stored.pack_revision;
        pinned.sha256 = stored.sha256;
        page.snapshot.push_back(std::move(pinned));
    }
    page.items.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const StoredWordSessionItem& source = items[offset + i];
        if (source.ordinal != offset + i) return ESP_ERR_INVALID_STATE;
        protocol::word_study_v1::SessionItem target;
        target.item_id = source.item_id;
        target.deck_id = source.deck_id;
        target.ordinal = source.ordinal;  // segment-local, continuous
        page.items.push_back(std::move(target));
    }
    *out = std::move(page);
    return ESP_OK;
}

esp_err_t WordSequenceAbsoluteCursor(
    const PersistedWordSession& session,
    uint64_t* absolute)
{
    if (absolute == nullptr) return ESP_ERR_INVALID_ARG;
    *absolute = 0;
    const auto& items = session.remote.items;
    if (session.position > items.size()) return ESP_ERR_INVALID_STATE;

    uint64_t ordinal = 0;
    if (session.position < items.size()) {
        const uint64_t value = items[session.position].ordinal;
        if (value > std::numeric_limits<uint32_t>::max()) {
            return ESP_ERR_INVALID_SIZE;
        }
        ordinal = value;
    } else {
        ordinal = items.empty() ? 0
                                : static_cast<uint64_t>(
                                      items.back().ordinal) + 1U;
        if (ordinal > std::numeric_limits<uint32_t>::max()) {
            return ESP_ERR_INVALID_SIZE;
        }
    }
    const uint64_t base = static_cast<uint64_t>(session.start_index);
    if (base > std::numeric_limits<uint64_t>::max() - ordinal) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint64_t result = base + ordinal;  // start_index added ONCE
    if (result > std::numeric_limits<uint32_t>::max()) {
        return ESP_ERR_INVALID_SIZE;
    }
    *absolute = result;
    return ESP_OK;
}

bool LocalSequenceSnapshotMatches(
    const WordPackIndex& index,
    const LocalWordSessionDescriptor& descriptor)
{
    if (!index.mounted || !index.has_manifest || index.truncated ||
        index.pack_error || index.entries.empty()) {
        return false;
    }
    if (!DescriptorSemanticsValid(descriptor)) return false;
    const auto& stored = descriptor.session.remote.snapshot;

    // (1) Every pinned deck's identity must still match a usable current pin.
    for (size_t i = 0; i < stored.size(); ++i) {
        const auto pin = std::find_if(
            index.pack_identities.begin(), index.pack_identities.end(),
            [&](const WordPackIdentity& value) {
                return FixedTextLength(value.deck_id, sizeof(value.deck_id), 36) &&
                    std::memcmp(value.deck_id, stored[i].deck_id, 36) == 0;
            });
        if (pin == index.pack_identities.end() || !ValidPin(*pin)) return false;
        if (pin->content_revision != stored[i].content_revision ||
            pin->pack_revision != stored[i].pack_revision ||
            std::strcmp(pin->sha256, stored[i].sha256) != 0) {
            return false;  // same size, different sha / revision => mismatch
        }
    }

    // (2) Re-derive the canonical cross-deck traversal order from the CURRENT
    // index (using the exact candidate comparator) and require the deck
    // SEQUENCE + per-deck word COUNTS to equal the pinned traversal order.
    // A same-size/same-SHA reorder (e.g. a pinned rebuild that renumbers
    // deck_order) is a mismatch, preventing a 500-boundary skip/repeat.
    ScratchVector<OrderedWord> ordered;
    ordered.reserve(index.entries.size());
    for (const WordPackIndexEntry& entry : index.entries) {
        if (!FixedTextLength(entry.word_id, sizeof(entry.word_id), 36) ||
            !FixedTextLength(entry.deck_id, sizeof(entry.deck_id), 36)) {
            return false;
        }
        ordered.push_back({&entry});
    }
    std::sort(ordered.begin(), ordered.end(), LocalCandidateLess);

    // Restrict to the pinned scope decks (in stored order is irrelevant here;
    // membership is by deck_id).
    ScratchVector<OrderedWord> scoped;
    for (const OrderedWord& ow : ordered) {
        bool in_scope = false;
        for (const auto& deck : descriptor.session.remote.deck_ids) {
            if (std::strcmp(deck.value, ow.entry->deck_id) == 0) {
                in_scope = true;
                break;
            }
        }
        if (in_scope) scoped.push_back(ow);
    }
    ScratchVector<TraversalDeck> current;
    if (DeriveTraversalOrder(scoped, 0, scoped.size(), &current) != ESP_OK) {
        return false;
    }
    // The descriptor does not store per-deck counts, but the candidate slice at
    // the pinned start must still be reproducible.  Compare the current
    // traversal deck SEQUENCE against the pinned deck_ids sequence, and require
    // the total scoped count to equal total_count (so a reordered/rebuilt index
    // that still has the same SHA set but a different deck order is caught).
    if (current.size() != descriptor.session.remote.deck_ids.size() ||
        (descriptor.session.remote.mode == Mode::kShuffle
            ? scoped.size() < descriptor.total_count
            : scoped.size() != descriptor.total_count)) {
        return false;
    }
    for (size_t i = 0; i < current.size(); ++i) {
        if (std::strcmp(current[i].deck_id,
                        descriptor.session.remote.deck_ids[i].value) != 0) {
            return false;  // traversal order changed
        }
    }
    return true;
}

}  // namespace wqn
