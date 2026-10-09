#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>
#include "device_protocol/word_study.h"
constexpr int ESP_OK=0, ESP_FAIL=1, ESP_ERR_INVALID_ARG=2, ESP_ERR_INVALID_STATE=3;
constexpr int ESP_ERR_INVALID_RESPONSE=4, ESP_ERR_INVALID_SIZE=5;
constexpr char kTag[]="host";
void Log(const char*, const char*, ...) {}
#define ESP_LOGI(...) Log(__VA_ARGS__)
#define ESP_ERROR_CHECK_WITHOUT_ABORT(value) do { (void)(value); } while(false)
namespace wqn::protocol::word_study_v1 {
@@POLICY_NAME@@
}
namespace wqn {
template<typename T> using WordStorePsramAllocator=std::allocator<T>;
enum class WordPresentationPhase : uint8_t { kFront=0,kBack=1 };
@@STRUCTS@@
@@EXTEND@@
struct WordAppState {
    struct Session {
        PersistedWordSession persisted;
        bool page_in_flight=true, page_requested=true;
        std::vector<std::string> buffered_ids{"RAM-event-70", "RAM-event-71"};
    } session;
    std::string message;
};
int card_reads=0, shown=0, pruned=0, requests=0, finishes=0, saves=0;
esp_err_t card_result=ESP_OK;
const char* WordPageExtendMessage(esp_err_t) { return "extension-error"; }
void PruneSessionOrdinals(WordAppState*) { ++pruned; }
esp_err_t LoadCurrentReviewWord(WordAppState*) { ++card_reads; return card_result; }
void ShowStudyCard(WordAppState*) { ++shown; }
void RequestCandidatePageIfNeeded(WordAppState*) { ++requests; }
void FinishOrLoadAdvancedReview(WordAppState*) { ++finishes; }
esp_err_t SavePersistedWordSession(const PersistedWordSession&) { ++saves; return ESP_OK; }
@@REDUCER@@
}
using namespace wqn;
using Page=protocol::word_study_v1::CandidatePageData;
int passes=0,failures=0;
void Check(bool okay,const char* label) {
    std::printf("%s: %s\n",okay?"PASS":"FAIL",label); okay?++passes:++failures;
}
PersistedWordSession Initial() {
    PersistedWordSession s; s.active=true; s.deck_scope_generation=7;
    auto& r=s.remote; r.session_id="aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
    r.ordering=protocol::word_study_v1::Ordering::kDueQueueV1;
    r.seed="pinned-seed"; r.cursor="cursor-32"; r.progress_revision=8; r.has_more=true;
    r.next_sequence=70;
    StoredWordPackSnapshot snapshot;
    std::snprintf(snapshot.deck_id,sizeof(snapshot.deck_id),"%s","bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb");
    std::snprintf(snapshot.sha256,sizeof(snapshot.sha256),"%s",std::string(64,'c').c_str());
    snapshot.content_revision=4; snapshot.pack_revision=5; r.snapshot.push_back(snapshot);
    for(uint64_t i=0;i<32;++i) {
        StoredWordSessionItem item; item.ordinal=i;
        std::snprintf(item.item_id,sizeof(item.item_id),"%08u-aaaa-aaaa-aaaa-aaaaaaaaaaaa",unsigned(i));
        std::snprintf(item.deck_id,sizeof(item.deck_id),"%s",snapshot.deck_id);
        r.items.push_back(item);
    }
    return s;
}
Page Next(const PersistedWordSession& s) {
    Page page; const auto& r=s.remote;
    page.session_id=r.session_id; page.seed=r.seed; page.ordering=r.ordering;
    page.candidate_policy_version=protocol::word_study_v1::CandidatePolicyVersionName(r.ordering);
    page.progress_revision=r.progress_revision; page.cursor=r.cursor;
    page.next_cursor="cursor-96"; page.has_more=true;
    for(const auto& stored:r.snapshot) {
        protocol::word_study_v1::PackSnapshot snapshot;
        snapshot.deck_id=stored.deck_id; snapshot.content_revision=stored.content_revision;
        snapshot.pack_revision=stored.pack_revision; snapshot.sha256=stored.sha256;
        page.snapshot.push_back(snapshot);
    }
    for(uint64_t i=32;i<96;++i) {
        protocol::word_study_v1::SessionItem item; item.ordinal=i;
        char id[37]; std::snprintf(id,sizeof(id),"%08u-aaaa-aaaa-aaaa-aaaaaaaaaaaa",unsigned(i));
        item.item_id=id; item.deck_id=r.snapshot.front().deck_id; page.items.push_back(item);
    }
    return page;
}
WordAppState State(bool advanced) {
    card_reads=shown=pruned=requests=finishes=saves=0; card_result=ESP_OK;
    WordAppState state; state.session.persisted=Initial();
    if(advanced) {
        state.session.persisted.position=3;
        state.session.persisted.remote.next_sequence=72;
        state.session.persisted.phase=WordPresentationPhase::kBack;
    }
    return state;
}
bool Unchanged(const WordAppState& s,bool advanced) {
    const auto& p=s.session.persisted;
    return p.remote.items.size()==32 && p.position==(advanced?3u:0u) &&
        p.remote.next_sequence==(advanced?72u:70u) && p.remote.cursor=="cursor-32" &&
        s.session.buffered_ids==std::vector<std::string>({"RAM-event-70", "RAM-event-71"}) &&
        card_reads==0 && shown==0 && pruned==0 && requests==0 && finishes==0 && saves==0;
}
int main() {
    const auto initial=Initial(); const auto page=Next(initial);
    PersistedWordSession runner;
    Check(ExtendPersistedWordSessionWithPage(initial,page,&runner)==ESP_OK &&
          runner.remote.items.size()==96,"fixture uses the production page validator and extender");
    for(bool advanced:{false,true}) {
        auto state=State(advanced);
        ApplyWordCandidatePageResult(&state,ESP_OK,ESP_OK,ESP_FAIL,runner,page);
        Check(Unchanged(state,advanced) && !state.session.page_in_flight && state.message=="后续单词未保存",
              advanced?"RAM-ahead path rejects failed snapshot save before exposing candidates":"current path rejects failed snapshot save");
        state=State(advanced);
        ApplyWordCandidatePageResult(&state,ESP_OK,ESP_ERR_INVALID_RESPONSE,ESP_OK,runner,page);
        Check(Unchanged(state,advanced) && state.message=="extension-error",
              advanced?"RAM-ahead path cannot reinterpret failed runner validation as saved":"current path retains validation error");
        state=State(advanced);
        ApplyWordCandidatePageResult(&state,ESP_FAIL,ESP_OK,ESP_OK,runner,page);
        Check(Unchanged(state,advanced) && !state.session.page_requested,"network failure preserves the original window and RAM identities");
        state=State(advanced);
        ApplyWordCandidatePageResult(&state,ESP_OK,ESP_OK,ESP_OK,{},page);
        Check(Unchanged(state,advanced) && state.message=="后续单词未保存",
              "default-success flags without a durable runner snapshot are not evidence of a save");
        state=State(advanced); auto wrong=runner; wrong.remote.session_id="other-session";
        ApplyWordCandidatePageResult(&state,ESP_OK,ESP_OK,ESP_OK,wrong,page);
        Check(Unchanged(state,advanced),"a different SID cannot supply the durable-page proof");
        state=State(advanced); wrong=runner; wrong.deck_scope_generation++;
        ApplyWordCandidatePageResult(&state,ESP_OK,ESP_OK,ESP_OK,wrong,page);
        Check(Unchanged(state,advanced),"a different scope cannot supply the durable-page proof");
        state=State(advanced);
        ApplyWordCandidatePageResult(&state,ESP_OK,ESP_OK,ESP_OK,runner,page);
        const auto& installed=state.session.persisted;
        Check(installed.remote.items.size()==(advanced?93u:96u) && installed.position==0 &&
              installed.remote.next_sequence==(advanced?72u:70u) &&
              installed.phase==(advanced?WordPresentationPhase::kBack:WordPresentationPhase::kFront) &&
              installed.remote.items.front().ordinal==(advanced?3u:0u) && card_reads==1 && shown==1 &&
              saves==0 && state.session.buffered_ids.size()==2,
              advanced?"successful stale page preserves RAM progress/phase/tail and performs no new UI save":"successful current page installs the already durable window without UI save");
    }
    auto state=State(true); state.session.persisted.paused=true;
    ApplyWordCandidatePageResult(&state,ESP_OK,ESP_OK,ESP_OK,runner,page);
    Check(Unchanged(state,true) && state.session.persisted.paused,"late page cannot revive a paused interaction");
    state=State(true); state.session.persisted.active=false;
    ApplyWordCandidatePageResult(&state,ESP_OK,ESP_OK,ESP_OK,runner,page);
    Check(Unchanged(state,true) && !state.session.persisted.active,"late page cannot revive an inactive interaction");
    state=State(true); card_result=ESP_FAIL;
    ApplyWordCandidatePageResult(&state,ESP_OK,ESP_OK,ESP_OK,runner,page);
    Check(state.message=="会话词包不可用" && shown==0 && saves==0,"card-read error is not hidden by the durable-page guard");
    std::printf("%d PASS / %d FAIL (production candidate reducer/extender, host UI/I/O seams; NOT HIL)\n",passes,failures);
    return failures?1:0;
}
