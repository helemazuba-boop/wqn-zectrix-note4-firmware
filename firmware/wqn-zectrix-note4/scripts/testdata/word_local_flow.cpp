// End-to-end local worker regression using the real durable file/journal code.
#define main DescriptorCodecFixtureMain
#include "@@BASE@@"
#undef main
#include <atomic>
#include <set>
namespace wqn { @@DECK@@ }
@@WIRE@@
namespace {
wqn::WordPackIndex current_index, pinned_index;
bool pin_available = true;
WordCloudRequest queued;
bool QueueWordCloudRequest(const WordCloudRequest& r) { queued = r; return true; }
}
namespace wqn {
esp_err_t LoadWordPackIndex(WordPackIndex* out) { *out=current_index; return ESP_OK; }
esp_err_t LoadWordPackIndexForSession(const PersistedWordSession&, WordPackIndex* out)
{ if(!pin_available) return ESP_ERR_NOT_FOUND; *out=pinned_index; return ESP_OK; }
}
@@QUEUE@@
@@WORKER@@

static wqn::WordPackIndex Index3507()
{
    wqn::WordPackIndex index;
    index.mounted=true; index.has_manifest=true;
    // Traversal order deliberately differs from UUID order.
    for (uint32_t d=0; d<3; ++d) {
        wqn::WordPackIdentity pin{};
        std::snprintf(pin.deck_id,sizeof(pin.deck_id),"%s",Uuid(300-d).c_str());
        pin.content_revision=1; pin.pack_revision=1;
        std::snprintf(pin.sha256,sizeof(pin.sha256),"%s",Sha('a'+d).c_str());
        index.pack_identities.push_back(pin);
    }
    for (uint32_t i=0; i<3507; ++i) {
        wqn::WordPackIndexEntry entry{};
        std::snprintf(entry.word_id,sizeof(entry.word_id),"%s",Uuid(100000+i).c_str());
        const auto& pin=index.pack_identities[i/1169];
        std::snprintf(entry.deck_id,sizeof(entry.deck_id),"%s",pin.deck_id);
        std::snprintf(entry.word,sizeof(entry.word),"word%04u",i);
        entry.deck_order=i/1169; entry.sort_index=i%1169;
        index.entries.push_back(entry);
    }
    return index;
}

static WordCloudResult Start(uint32_t n, uint32_t base)
{
    wqn::protocol::word_study_v1::CreateSessionRequest request;
    request.metadata.request_id="flow-start-"+std::to_string(n);
    request.mode=wqn::protocol::word_study_v1::Mode::kSequential;
    request.start_index=base;
    if(!QueueWordSessionStart(request,false)) return {};
    WordCloudResult result;
    result.scope_generation=queued.scope_generation;
    ExecuteLocalWordStart(queued,&result);
    return result;
}

static bool Prefetch(wqn::PersistedWordSession* session)
{
    wqn::protocol::word_study_v1::CandidatePageRequest page;
    page.metadata.request_id="flow-page-"+session->remote.cursor;
    page.cursor=session->remote.cursor; page.limit=64;
    if(!QueueWordCandidatePage(session->remote.session_id,page,*session) ||
       queued.study_mode!=static_cast<uint8_t>(session->remote.mode)) return false;
    WordCloudResult result;
    ExecuteLocalWordPage(queued,&result);
    if(result.result!=ESP_OK) { std::printf("page error=%d cursor=%s pos=%u size=%zu\n",result.result,page.cursor.c_str(),session->position,session->remote.items.size()); return false; }
    *session=std::move(result.persisted_session);
    return session->remote.items.size()<=96;
}

int main(int argc,char** argv)
{
    if(argc<2) return 2;
    g_storage_root=argv[1]; CleanStorage();
    current_index=Index3507(); pinned_index=current_index;
    auto start=Start(0,0);
    Check(start.result==ESP_OK && start.persisted_session.remote.items.size()==32,
          "real worker starts offline with 32 candidates");
    if(start.result!=ESP_OK) return 1;
    auto session=start.persisted_session;
    const auto initial_sid=session.remote.session_id;
    Check(Prefetch(&session) && session.remote.items.size()==96,
          "real local queue routes page by mode and fills at most 96 candidates");
    std::set<std::string> seen;
    bool walked=true, durable=true, stable_pin=true, unique_segments=true;
    size_t largest_window=session.remote.items.size();
    std::set<std::string> segment_ids{session.remote.session_id};
    uint32_t segment=0;
    for(uint32_t absolute=0;absolute<3507;++absolute) {
        if(session.position>=session.remote.items.size()) { walked=false; break; }
        auto item=session.remote.items[session.position];
        uint64_t cursor=0;
        if(wqn::WordSequenceAbsoluteCursor(session,&cursor)!=ESP_OK || cursor!=absolute ||
           item.item_id!=Uuid(100000+absolute) || !seen.insert(item.item_id).second) {
            walked=false; break;
        }
        std::vector<wqn::DurableWordObservation> observations;
        for(int phase=0;phase<2;++phase) {
            wqn::DurableWordObservation fact;
            fact.request_id="flow-fact-"+std::to_string(absolute*2+phase);
            fact.session_id=session.remote.session_id;
            fact.item_id=item.item_id;
            fact.sequence=session.remote.next_sequence++;
            fact.mode=session.remote.mode;
            fact.action=phase==0 ? wqn::protocol::word_study_v1::ObservationAction::kRevealed
                                : wqn::protocol::word_study_v1::ObservationAction::kKnown;
            fact.occurred_at="2026-11-05T00:00:00.000Z";
            if(phase==1) ++session.position;
            session.phase=phase==0?wqn::WordPresentationPhase::kBack:wqn::WordPresentationPhase::kFront;
            session.active=session.position<session.remote.items.size() || session.remote.has_more;
            fact.next_position=item.ordinal+(phase==1?1:0);
            fact.next_phase=session.phase;
            observations.push_back(fact);
        }
        if(wqn::CommitWordObservations(observations,session)!=ESP_OK) { durable=false; break; }
        // Reset at card 499, each segment boundary and later window positions.
        if(absolute==498 || absolute==499 || absolute==3498 || absolute==3499 || absolute==3506) {
            WqnHostResetWordOutboxCache();
            wqn::PersistedWordSession reboot;
            if(wqn::LoadPersistedWordSession(session.remote.mode,&reboot)!=ESP_OK ||
               wqn::WordSequenceAbsoluteCursor(reboot,&cursor)!=ESP_OK || cursor!=absolute+1 ||
               reboot.remote.next_sequence!=session.remote.next_sequence) { durable=false; break; }
            session=std::move(reboot);
        }
        if(absolute==750) {
            // New manifest is installed while the old round is still pinned.
            current_index.pack_identities[0].sha256[0]='f';
        }
        if(session.remote.has_more && session.remote.items.size()-session.position<=32) {
            if(!Prefetch(&session)) { walked=false; break; }
            largest_window=std::max(largest_window,session.remote.items.size());
        }
        if(!session.active && absolute+1<3507) {
            auto next=Start(++segment,absolute+1);
            if(next.result!=ESP_OK || next.local_segment_start!=absolute+1) { walked=false; break; }
            session=std::move(next.persisted_session);
            unique_segments &= segment_ids.insert(session.remote.session_id).second;
            stable_pin &= session.remote.snapshot[0].sha256[0]=='a';
            if(session.remote.has_more && !Prefetch(&session)) { walked=false; break; }
        }
    }
    std::printf("walk status seen=%zu segments=%zu durable=%d position=%u items=%zu base=%u active=%d\n",seen.size(),segment_ids.size(),durable,session.position,session.remote.items.size(),session.start_index,session.active);
    Check(walked && seen.size()==3507,"3507 real-worker cards have zero skips or repeats");
    Check(durable,"real session loads replay journal at 499/500/3499/3500/3507");
    Check(largest_window==96,"96-item window bound holds for the full round");
    Check(segment_ids.size()==8 && unique_segments,"all 8 segments have distinct stable identities");
    Check(stable_pin,"background manifest update cannot reorder the active round");
    wqn::WordOutboxSnapshot outbox;
    Check(wqn::ReadWordOutboxSnapshot(&outbox)==ESP_OK && outbox.pending_count==7014 &&
          outbox.capacity==8192,"all 7014 facts remain durable within the 8192-record capacity");
    auto second=Start(99,0);
    Check(second.result==ESP_OK && second.local_segment_start==0 &&
          second.persisted_session.remote.items[0].item_id==Uuid(100000) &&
          second.persisted_session.remote.session_id!=initial_sid,
          "true completion starts a second round at zero with a fresh SID");
    Check(second.result==ESP_OK && second.persisted_session.remote.snapshot[0].sha256[0]=='f',
          "second round adopts the new validated pack identity");
    // The second round can be recovered exactly without a cloud token.
    if(second.result==ESP_OK) {
        session=second.persisted_session;
        wqn::DurableWordObservation reveal;
        reveal.request_id="second-round-reveal";
        reveal.session_id=session.remote.session_id;
        reveal.item_id=session.remote.items[0].item_id;
        reveal.sequence=session.remote.next_sequence++;
        reveal.action=wqn::protocol::word_study_v1::ObservationAction::kRevealed;
        reveal.occurred_at="2026-11-05T00:00:01.000Z";
        reveal.next_phase=wqn::WordPresentationPhase::kBack;
        session.phase=wqn::WordPresentationPhase::kBack;
        Check(wqn::CommitWordObservation(reveal,session)==ESP_OK,
              "second-round back phase is a real journal fact");
        session.paused=true;
        const auto saved=wqn::SaveWordSessionCursor(session);
        WqnHostResetWordOutboxCache();
        wqn::PersistedWordSession reboot;
        Check(wqn::LoadPersistedWordSession(session.remote.mode,&reboot)==ESP_OK &&
              reboot.paused && reboot.phase==wqn::WordPresentationPhase::kBack,
              "fresh file scan restores durable pause and back phase");
        pinned_index=current_index;
        auto resume=Start(100,0);
        Check(saved==ESP_OK && resume.result==ESP_OK &&
              resume.persisted_session.remote.session_id==session.remote.session_id &&
              resume.persisted_session.phase==wqn::WordPresentationPhase::kBack &&
              !resume.persisted_session.paused,"paused back-phase session resumes its original SID offline");
        Check(wqn::LoadPersistedWordSession(session.remote.mode,&reboot)==ESP_OK && !reboot.paused,
              "worker durably clears paused flag before publishing resume");
    }
    // A missing historical pin requires a safe, explicit whole-round restart.
    pin_available=false;
    current_index.pack_identities[0].sha256[0]='e';
    auto restart=Start(101,42);
    Check(restart.result==ESP_OK && restart.local_segment_start==0 &&
          restart.message=="词库已更新，已从第一词开始",
          "unavailable old pin plus changed manifest restarts at zero with notice");
    std::printf("%d PASS / %d FAIL (real local worker/store/files; host scheduling and NVS seams, NOT HIL)\n",passes,failures);
    return failures?1:0;
}
