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
namespace {
bool network_online=false;
bool eligibility_supported=true;
size_t eligibility_requests=0;
std::string actor_token(64,'a');
std::set<std::string> mastered_ids;
}
namespace wqn {
esp_err_t LoadAccessToken(std::string* out){*out=actor_token;return ESP_OK;}
bool IsValidAccessToken(const std::string& token){return token.size()==64;}
namespace services {
bool IsConnectivityOnline(){return network_online;}
protocol::v3::RequestMetadata MakeDeviceRequestMetadata(){protocol::v3::RequestMetadata m;m.request_id="eligibility-fixture-request";return m;}
}
esp_err_t FetchWordEligibilityV1(const std::string&,
    const protocol::word_study_v1::EligibilityRequest& request,
    protocol::word_study_v1::EligibilityData* out,protocol::v3::Error*, int* status){
    if(status)*status=eligibility_supported?200:404;
    if(!eligibility_supported)return ESP_ERR_NOT_SUPPORTED;
    ++eligibility_requests;
    std::vector<std::string> ids;
    for(const auto& e:current_index.entries){
        if(std::any_of(request.snapshot.begin(),request.snapshot.end(),[&](const auto& p){return p.deck_id==e.deck_id;}))ids.emplace_back(e.word_id);
    }
    std::sort(ids.begin(),ids.end());std::string canonical;
    *out={};out->progress_revision=41;
    for(size_t i=0;i<ids.size();++i){if(i)canonical+='\n';canonical+=ids[i];out->mastered_bitmap+=mastered_ids.count(ids[i])?'1':'0';}
    out->item_ids_sha256=WordLocalSha256(canonical);return ESP_OK;
}
}
@@QUEUE@@
@@REFRESH@@
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
    // A full second scenario uses the same real worker and native file/journal
    // implementation. The only eligibility source is the fake authenticated
    // HTTP seam above; the actual cache writer/loader are production code.
    CleanStorage();WqnHostResetWordOutboxCache();
    current_index=Index3507();pinned_index=current_index;pin_available=true;
    for(uint32_t i=0;i<17;++i)mastered_ids.insert(Uuid(100000+i*7));
    auto random_start=[&](uint32_t number){
        wqn::protocol::word_study_v1::CreateSessionRequest req;
        req.mode=wqn::protocol::word_study_v1::Mode::kShuffle;
        req.metadata.request_id="shuffle-start-"+std::to_string(number);req.start_index=0;
        QueueWordSessionStart(req,false);WordCloudResult result;result.scope_generation=queued.scope_generation;
        ExecuteLocalWordStart(queued,&result);return result;
    };
    auto missing=random_start(0);
    Check(missing.result!=ESP_OK,"fresh offline shuffle requires personal learning state, never assumes all words are new");
    network_online=true;eligibility_supported=false;
    auto legacy=random_start(9010);
    Check(legacy.result!=ESP_OK && legacy.legacy_online_fallback,
        "old cloud without eligibility endpoint preserves fresh online shuffle fallback");
    network_online=false;eligibility_supported=true;
    wqn::LocalWordEligibility latest;
    Check(RefreshLocalWordEligibility(current_index,"",g_scope_generation,std::string(64,'a'),&latest)==ESP_OK,
        "complete personal bitmap persists separately from shared content");
    auto changed_ids=current_index;
    std::snprintf(changed_ids.entries[0].word_id,sizeof(changed_ids.entries[0].word_id),"%s",Uuid(999999).c_str());
    Check(!wqn::LocalWordEligibilityMatches(changed_ids,latest,""),
        "same-size changed item set cannot reuse personal bitmap bits");
    auto stale=latest;stale.deck_scope_generation=g_scope_generation-1;
    Check(wqn::SaveLocalWordEligibility(stale)==ESP_ERR_INVALID_STATE,
        "stale scope cannot publish a progress cache");
    actor_token.assign(64,'b');
    Check(random_start(9000).result!=ESP_OK,"another paired actor cannot use the previous personal progress cache");
    actor_token.assign(64,'a');
    const auto calls=eligibility_requests;
    auto random=random_start(1);
    Check(random.result==ESP_OK && random.persisted_session.remote.mode==wqn::protocol::word_study_v1::Mode::kShuffle &&
        random.local_round_total==3490,"offline shuffle excludes all 17 mastered words");
    if(random.result==ESP_OK){
        session=random.persisted_session;
        const auto round_seed=session.remote.seed;
        wqn::LocalWordEligibility immutable;
        Check(wqn::LoadLocalWordEligibility(round_seed,&immutable)==ESP_OK,"round bitmap reloads from its CRC-protected immutable file");
        auto changed=immutable;changed.mastered_bitmap[0]=changed.mastered_bitmap[0]=='0'?'1':'0';
        Check(wqn::SaveLocalWordEligibility(changed)==ESP_ERR_INVALID_STATE,
            "the same round identity cannot be rebound to a different mastered set");

        const auto first_random_word=std::string(session.remote.items[0].item_id);
        std::set<std::string> random_seen,random_sids{session.remote.session_id};
        bool complete=true,recovered=true,seed_stable=true,phase_stable=true;
        for(uint32_t absolute=0;absolute<3490;++absolute){
            if(session.position>=session.remote.items.size()){
                if(!Prefetch(&session)){complete=false;break;}
            }
            auto item=session.remote.items[session.position];
            if(mastered_ids.count(item.item_id)||!random_seen.insert(item.item_id).second){complete=false;break;}
            for(int phase=0;phase<2;++phase){
                wqn::DurableWordObservation fact;
                fact.request_id="shuffle-fact-"+std::to_string(absolute*2+phase);
                fact.session_id=session.remote.session_id;fact.item_id=item.item_id;
                fact.mode=session.remote.mode;fact.sequence=session.remote.next_sequence++;
                fact.action=phase==0?wqn::protocol::word_study_v1::ObservationAction::kRevealed:wqn::protocol::word_study_v1::ObservationAction::kKnown;
                fact.occurred_at="2026-11-06T00:00:00Z";
                session.phase=phase==0?wqn::WordPresentationPhase::kBack:wqn::WordPresentationPhase::kFront;
                if(phase)++session.position;
                session.active=session.position<session.remote.items.size()||session.remote.has_more;
                fact.next_position=static_cast<uint32_t>(item.ordinal)+(phase?1:0);fact.next_phase=session.phase;
                if(wqn::CommitWordObservation(fact,session)!=ESP_OK){complete=false;break;}
                if(absolute==94 && phase==0){
                    session.paused=true;
                    phase_stable &= wqn::SaveWordSessionCursor(session)==ESP_OK;
                    WqnHostResetWordOutboxCache();
                    auto resumed=random_start(100);
                    phase_stable &= resumed.result==ESP_OK && resumed.persisted_session.remote.seed==round_seed &&
                        resumed.persisted_session.phase==wqn::WordPresentationPhase::kBack && !resumed.persisted_session.paused;
                    if(resumed.result==ESP_OK)session=resumed.persisted_session;
                }
            }
            if(!complete)break;
            if(absolute==95||absolute==498||absolute==499||absolute==3489){
                WqnHostResetWordOutboxCache();wqn::PersistedWordSession reboot;uint64_t cursor=0;
                recovered &= wqn::LoadPersistedWordSession(session.remote.mode,&reboot)==ESP_OK &&
                    wqn::WordSequenceAbsoluteCursor(reboot,&cursor)==ESP_OK && cursor==absolute+1;
                if(recovered)session=reboot;
            }
            if(!session.active && absolute+1<3490){
                // Refreshing latest state mid-round cannot modify the retained
                // per-round bitmap or reorder later 500-item segments.
                if(absolute==499){
                    latest.mastered_bitmap.assign(3507,'1');latest.progress_revision=42;
                    Check(wqn::SaveLocalWordEligibility(latest)==ESP_OK,"background progress update retains the original round bitmap");
                }
                auto next=random_start(200+absolute);
                if(next.result!=ESP_OK){complete=false;break;}
                session=next.persisted_session;random_sids.insert(session.remote.session_id);
                seed_stable &= session.remote.seed==round_seed && session.remote.progress_revision==41;
            }else if(session.position==session.remote.items.size()&&session.remote.has_more){
                if(!Prefetch(&session)){complete=false;break;}
            }
        }
        Check(complete && random_seen.size()==3490 && random_sids.size()==7,
            "3490 offline random words cross seven real segments with no duplicates or omissions");
        Check(seed_stable && recovered && phase_stable,"shuffle keeps its round seed, exact cursor and paused back phase across reboot");
        Check(eligibility_requests==calls,"offline start, paging, pause and segment continuation make zero HTTP requests");
        Check(first_random_word!=Uuid(100001),"random order is distinct from sequential deck order");
        wqn::WordOutboxSnapshot outbox;
        Check(wqn::ReadWordOutboxSnapshot(&outbox)==ESP_OK && outbox.pending_count==6980,
            "shuffle retains all 6980 individual durable learning facts");
        auto all_mastered=random_start(10000);
        Check(all_mastered.result==ESP_OK && all_mastered.local_round_complete,
            "new round uses refreshed eligibility without silently including mastered words");
    }
    std::printf("%d PASS / %d FAIL (real local worker/store/files; host scheduling and NVS seams, NOT HIL)\n",passes,failures);
    return failures?1:0;
}
