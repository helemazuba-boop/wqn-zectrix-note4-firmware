#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_INVALID_ARG=1, ESP_ERR_NOT_FOUND=2;
constexpr char kTag[]="test";
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
unsigned passed=0, failed=0, reads=0, scope=3;
std::atomic<uint32_t>* cleanup_state=nullptr;
bool free_during_cleanup=false;
void Check(bool value, const char* name) {
    std::printf("%s: %s\n", value?"PASS":"FAIL", name);
    ++(value?passed:failed);
}
namespace wqn {
struct WordPackIndexEntry {
    char word_id[37]{}, deck_id[37]{}, pack_stem[28]{};
    uint32_t file_offset=0;
};
struct WqnWordEntry {
    std::string id, deck_id, word;
    WqnWordEntry(std::string a="", std::string b="", std::string c="") :
        id(std::move(a)), deck_id(std::move(b)), word(std::move(c)) {}
    WqnWordEntry(const WqnWordEntry&)=default;
    WqnWordEntry(WqnWordEntry&&)=default;
    WqnWordEntry& operator=(const WqnWordEntry& value) {
        if (cleanup_state && value.id.empty() && cleanup_state->load()==0) free_during_cleanup=true;
        id=value.id; deck_id=value.deck_id; word=value.word; return *this;
    }
    WqnWordEntry& operator=(WqnWordEntry&& value) {
        if (cleanup_state && value.id.empty() && cleanup_state->load()==0) free_during_cleanup=true;
        id=std::move(value.id); deck_id=std::move(value.deck_id); word=std::move(value.word); return *this;
    }
};
enum class WordAppMode { kHome, kWordCard };
struct Item { char item_id[37]{}, deck_id[37]{}; uint64_t ordinal=0; };
struct Remote { std::string session_id; std::vector<Item> items; };
struct Session { bool active=true; uint32_t position=0, deck_scope_generation=3; Remote remote; };
@@STATE@@
struct WordAppState {
    WordAppMode mode=WordAppMode::kWordCard;
    struct { Session persisted; } session;
    struct { bool replay_in_flight=false; uint64_t replay_return_ordinal=0; } review;
    struct { std::vector<WordPackIndexEntry> entries; } pack_index;
    WqnWordEntry current_word;
    WordCardPrefetchState card_prefetch;
};
unsigned GetDeckScopeGeneration() { return scope; }
size_t SessionIndexOfOrdinal(const Remote& r, uint64_t ordinal) {
    for (size_t i=0; i<r.items.size(); ++i) if (r.items[i].ordinal==ordinal) return i;
    return r.items.size();
}
size_t NextQueuePosition(const WordAppState&, size_t pos) { return pos; }
bool SameWordCardPrefetchIndex(const WordPackIndexEntry&, const WordPackIndexEntry&);
esp_err_t ReadWordPackEntry(const WordPackIndexEntry& index, WqnWordEntry* entry) {
    ++reads; *entry={index.word_id,index.deck_id,"disk"}; return ESP_OK;
}
@@CACHE_READ@@
@@REDUCERS@@
}
enum class PersistKind { kWordObservation, kWordCardPrefetch, kCount };
enum SlotState : uint32_t { kSlotFree, kSlotFilling, kSlotQueued, kSlotRunning, kSlotResultPending, kSlotAcknowledging };
struct PersistResultReceipt { esp_err_t result=ESP_FAIL; uint32_t operation_id=0, generation=0; };
struct PersistCommand {
    std::atomic<uint32_t> state{kSlotFree};
    PersistKind kind=PersistKind::kWordObservation;
    uint32_t operation_id=0;
    int word_obs=0, word_advanced=0, note_obs=0, note_advanced=0, problem_obs=0, settings_int=0;
    std::vector<int> word_batch;
    wqn::WordPackIndexEntry word_card_index{};
    wqn::WqnWordEntry word_card_result;
    std::string settings_str;
};
struct PersistMailbox {
    std::atomic<uint32_t> pending_generation{0}, acked_generation{0};
    esp_err_t result=ESP_OK;
    uint32_t operation_id=0;
    uint8_t slot_index=0;
};
constexpr size_t kPoolDepth=2;
PersistCommand g_pool[kPoolDepth];
PersistMailbox g_mailbox[2];
bool busy=true;
bool ValidKind(PersistKind kind) { return kind<PersistKind::kCount; }
size_t KindIndex(PersistKind kind) { return static_cast<size_t>(kind); }
void ReleaseKind(PersistKind) { busy=false; }
@@CLEANUP@@
@@MAILBOX@@
wqn::WordAppState Fixture() {
    wqn::WordAppState s; s.session.persisted.remote.session_id="session";
    for (unsigned i=0; i<3; ++i) {
        wqn::Item item; std::snprintf(item.item_id,37,"word%u",i);
        std::snprintf(item.deck_id,37,"deck"); item.ordinal=i+1;
        s.session.persisted.remote.items.push_back(item);
        wqn::WordPackIndexEntry index;
        std::snprintf(index.word_id,37,"word%u",i); std::snprintf(index.deck_id,37,"deck");
        std::snprintf(index.pack_stem,28,"version1"); index.file_offset=100*i;
        s.pack_index.entries.push_back(index);
    }
    return s;
}
bool Ready(wqn::WordAppState* s) {
    wqn::WordPackIndexEntry index;
    return wqn::TakeWordCardPrefetchEntry(s,10,100,&index) &&
        wqn::ApplyWordCardPrefetchResult(s,10,ESP_OK,{index.word_id,index.deck_id,"RAM"},101);
}
int main() {
    using namespace wqn;
    auto s=Fixture(); WordPackIndexEntry index;
    Check(GetWordCardPrefetchEntry(s,0,&index) && std::string(index.word_id)=="word1","predict ordinary next without changing cursor");
    Check(s.session.persisted.position==0 && s.card_prefetch.operation_id==0,"peek has no side effects");
    Check(!TakeWordCardPrefetchEntry(&s,0,0,&index),"zero op cannot bind");
    Check(TakeWordCardPrefetchEntry(&s,10,0,&index),"bind only after reservation");
    Check(!GetWordCardPrefetchEntry(s,0,&index),"one in-flight read");
    Check(!ApplyWordCardPrefetchResult(&s,11,ESP_OK,{"word1","deck","bad"},0) && s.card_prefetch.operation_id==10,"stale op cannot consume current read");
    Check(ApplyWordCardPrefetchResult(&s,10,ESP_OK,{"word1","deck","RAM"},0),"matching result becomes ready");
    Check(!GetWordCardPrefetchEntry(s,0,&index),"ready key is not reread");
    s.session.persisted.position=1; reads=0;
    Check(LoadCurrentReviewWord(&s)==ESP_OK && reads==0 && s.current_word.word=="RAM","RAM hit performs no card I/O");
    Check(!s.card_prefetch.ready,"cache is consumed once");
    Check(LoadCurrentReviewWord(&s)==ESP_OK && reads==1,"consumed card falls back normally");
    s=Fixture(); Ready(&s); s.session.persisted.position=1;
    std::snprintf(s.pack_index.entries[1].pack_stem,28,"version2"); reads=0;
    Check(LoadCurrentReviewWord(&s)==ESP_OK && reads==1,"new pack version cannot use old cached bytes");
    s=Fixture(); Ready(&s); s.session.persisted.position=1; s.pack_index.entries[1].file_offset++; reads=0;
    Check(LoadCurrentReviewWord(&s)==ESP_OK && reads==1,"changed offset cannot hit");
    s=Fixture(); Ready(&s); s.session.persisted.position=1; s.session.persisted.remote.session_id="other"; reads=0;
    Check(LoadCurrentReviewWord(&s)==ESP_OK && reads==1,"other session cannot hit");
    s=Fixture(); Ready(&s); s.session.persisted.position=1; scope=4; reads=0;
    Check(LoadCurrentReviewWord(&s)==ESP_OK && reads==1,"live scope invalidates ready cache");
    Check(!GetWordCardPrefetchEntry(s,0,&index),"stale scope cannot start another read"); scope=3;
    s=Fixture(); TakeWordCardPrefetchEntry(&s,10,0,&index); scope=4;
    Check(!ApplyWordCardPrefetchResult(&s,10,ESP_OK,{"word1","deck","x"},0) && !s.card_prefetch.ready,"live scope discards delayed result"); scope=3;
    s=Fixture(); TakeWordCardPrefetchEntry(&s,10,0,&index); s.session.persisted.remote.session_id="new";
    Check(!ApplyWordCardPrefetchResult(&s,10,ESP_OK,{"word1","deck","x"},0),"new session discards delayed result");
    s=Fixture(); TakeWordCardPrefetchEntry(&s,10,0,&index); s.pack_index.entries[1].file_offset++;
    Check(!ApplyWordCardPrefetchResult(&s,10,ESP_OK,{"word1","deck","x"},0),"index replacement discards delayed result");
    s=Fixture(); TakeWordCardPrefetchEntry(&s,10,0,&index);
    Check(!ApplyWordCardPrefetchResult(&s,10,ESP_FAIL,{},100) && !GetWordCardPrefetchEntry(s,5099,&index),"read failure backs off five seconds");
    Check(GetWordCardPrefetchEntry(s,5100,&index),"read retry becomes eligible at boundary");
    s=Fixture(); TakeWordCardPrefetchEntry(&s,10,0,&index);
    Check(!ApplyWordCardPrefetchResult(&s,10,ESP_OK,{"wrong","deck","x"},0),"wrong content ID rejected");
    s=Fixture(); TakeWordCardPrefetchEntry(&s,10,0,&index);
    Check(!ApplyWordCardPrefetchResult(&s,10,ESP_OK,{"word1","other","x"},0),"wrong deck rejected");
    s=Fixture(); s.review.replay_in_flight=true; s.review.replay_return_ordinal=3;
    Check(GetWordCardPrefetchEntry(s,0,&index) && std::string(index.word_id)=="word2","replay predicts return without drawing RNG");
    s=Fixture(); s.session.persisted.position=2;
    Check(!GetWordCardPrefetchEntry(s,0,&index),"last card does not read beyond window");
    s.session.persisted.position=UINT32_MAX;
    Check(!GetWordCardPrefetchEntry(s,0,&index),"invalid position cannot wrap to first item");
    s=Fixture(); s.mode=WordAppMode::kHome;
    Check(!GetWordCardPrefetchEntry(s,0,&index),"home does not speculate");
    s=Fixture(); s.session.persisted.active=false;
    Check(!GetWordCardPrefetchEntry(s,0,&index),"inactive session does not speculate");
    s=Fixture(); auto other=s.pack_index.entries[1]; std::snprintf(other.deck_id,37,"other");
    s.pack_index.entries.insert(s.pack_index.entries.begin()+1,other); Ready(&s); s.session.persisted.position=1; reads=0;
    Check(LoadCurrentReviewWord(&s)==ESP_OK && reads==0 && s.current_word.deck_id=="deck","same word in another deck cannot steal RAM hit");
    auto& c=g_pool[0]; auto& box=g_mailbox[KindIndex(PersistKind::kWordCardPrefetch)];
    c.kind=PersistKind::kWordCardPrefetch; c.operation_id=77; c.word_card_result={"id","deck","owned"};
    c.word_obs=99; ClearCommandPayload(c,true);
    Check(c.word_obs==0 && c.word_card_result.word=="owned","worker cleanup retains owned read result only");
    c.state.store(kSlotResultPending); box.operation_id=77; box.pending_generation.store(9);
    PersistResultReceipt receipt; WqnWordEntry card;
    Check(TakeWordCardPrefetchResult(&receipt,&card) && card.word=="owned","published mailbox carries card");
    card={}; Check(TakeWordCardPrefetchResult(&receipt,&card) && card.word=="owned","repeat take cannot lose result before ACK");
    Check(!AckPersistResult(PersistKind::kWordCardPrefetch,8,77) && c.word_card_result.word=="owned","stale generation cannot free/clear");
    Check(!AckPersistResult(PersistKind::kWordCardPrefetch,9,76) && c.word_card_result.word=="owned","stale operation cannot free/clear");
    c.state.store(kSlotRunning);
    Check(!AckPersistResult(PersistKind::kWordCardPrefetch,9,77) && c.word_card_result.word=="owned","wrong slot state cannot clear result");
    c.state.store(kSlotResultPending);
    cleanup_state=&c.state;
    Check(AckPersistResult(PersistKind::kWordCardPrefetch,9,77) && c.state.load()==kSlotFree && !busy && c.word_card_result.id.empty(),"matching ACK clears payload and releases slot");
    cleanup_state=nullptr;
    Check(!free_during_cleanup,"slot is not reusable while read payload is being cleared");
    Check(!TakeWordCardPrefetchResult(&receipt,&card),"ACK watermark prevents repeat consumption");
    c.state.store(kSlotFilling); c.operation_id=88; c.word_card_result={"new","deck","next"};
    Check(!AckPersistResult(PersistKind::kWordCardPrefetch,9,77) && c.word_card_result.word=="next","duplicate old ACK cannot clear reused slot");
    std::printf("%u PASS / %u FAIL\n",passed,failed); return failed?1:0;
}
