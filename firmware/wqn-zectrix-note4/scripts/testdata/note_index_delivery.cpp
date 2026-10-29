#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include "note_app.h"
#include "note_cloud.h"
#include "note_cloud.cpp"

#define HAS_GENERATION @@HAS_GENERATION@@
using Manifest=wqn::WqnNotePackManifest;
Manifest stored,remote;
std::map<std::string,bool> files;
size_t io_calls=0,checks=0,failures=0,body_reads=0;
uint64_t body_revision=0;
int fail_index=0;
void Check(bool okay,const char* name)
{ ++checks;if(!okay){++failures;if(failures<30)std::fprintf(stderr,"FAIL %s\n",name);} }
namespace wqn {
static constexpr size_t kMaxStoredNotebooks=200;
@@MERGE@@
namespace services { protocol::v3::RequestMetadata MakeDeviceRequestMetadata(){return {};} }
esp_err_t FetchNoteStudyManifest(const std::string&,const protocol::v3::RequestMetadata&,uint64_t,
    Manifest* out,const std::string&,protocol::v3::Error*) { ++io_calls;*out=remote;return ESP_OK; }
esp_err_t LoadNotePackManifest(Manifest* out) {++io_calls;*out=stored;return ESP_OK;}
esp_err_t SaveNotePackManifest(const Manifest& value) {++io_calls;stored=value;return ESP_OK;}
esp_err_t ResetNotePackStorageCache() {++io_calls;stored={};return ESP_OK;}
esp_err_t EnsurePackDownloadCapacity(size_t,size_t) {++io_calls;return ESP_OK;}
bool NotePackNeedsDownload(const WqnNotePackManifestNotebook& item,const std::string*)
{++io_calls;return files.count(item.sha256)==0;}
esp_err_t DownloadNotePackToStorage(const std::string&,const protocol::v3::RequestMetadata&,
    const WqnNotePackManifestNotebook& item,WqnTransferProgressSink) {++io_calls;files[item.sha256]=true;return ESP_OK;}
esp_err_t LoadNotePackIndex(NotePackIndex* out) {
    ++io_calls;*out={};if(fail_index) return ESP_FAIL;
    out->mounted=out->has_manifest=true;out->manifest_revision=stored.cursor;
    for(const auto& row:stored.notebooks) {
        NotePackNotebook book;book.notebook_id=row.notebook_id;book.title=row.title;
        book.content_revision=row.content_revision;book.pack_revision=row.pack_revision;
        book.sha256=row.sha256;book.has_pack=true;out->notebooks.push_back(book);
    }
    return ESP_OK;
}
}
namespace {
// Body I/O/observation ports; actual apply logic decides whether to invoke them.
void LoadCurrentNoteBody(wqn::NoteAppState* state) {
    ++body_reads;state->current_note_loaded=!state->pack_index.notebooks.empty();
    body_revision=state->current_note_loaded?state->pack_index.notebooks.front().content_revision:0;
}
void ArmOpenObservation(wqn::NoteAppState*) {}
void ResetNoteImageViewer(wqn::NoteAppState*) {}
@@UI_HELPERS@@
@@INSTALL@@
@@ACTIVATE@@
}
namespace wqn {
@@APPLY@@
@@BODY_APPLY@@
struct UiState { NoteAppState note_app; };
}
namespace device_ui_internal {
enum class NoteCloudOp {kPackSync,kFetchNotebookPack};
struct NoteCloudResult { NoteCloudOp op=NoteCloudOp::kPackSync;int result=ESP_OK;
    bool pack_index_ready=false,auth_required=false;wqn::NotePackIndex pack_index;std::string fetched_notebook_id,message; };
using UiState=wqn::UiState;
void BuildHomeSummary(UiState*) {}
enum class CloudDomain {kNote,kNoteBulk};
constexpr size_t kCloudDomainCount=2;
void NotifyUiTask() {}
@@MAILBOX@@
@@UI_APPLY@@
}
using namespace device_ui_internal;
const std::string notebook(36,'a');
uint64_t Generation(const wqn::NotePackIndex& index) {
#if HAS_GENERATION
    return index.delivery_generation;
#else
    (void)index;return 0;
#endif
}
wqn::WqnNotePackManifestNotebook Row(uint64_t revision) {
    wqn::WqnNotePackManifestNotebook row;row.notebook_id=notebook;row.pack_id=notebook;
    row.title="v"+std::to_string(revision);row.content_revision=row.pack_revision=row.change_sequence=revision;
    row.has_pack=true;row.schema_version=1;row.entry_count=1;row.byte_size=20;row.sha256=std::string(64,'a'+revision%10);
    return row;
}
void Reset() {stored={};stored.notebooks={Row(0)};remote={};remote.snapshot_id=std::string(64,'e');
    body_reads=0;body_revision=0;
    files.clear();files[stored.notebooks[0].sha256]=true;io_calls=0;fail_index=0;
    for(auto& box:g_result_mailbox){box.pending_generation=0;box.acked_generation=0;}}
wqn::NotePackSyncResult Build(bool bulk,uint64_t revision) {
    remote.notebooks={Row(revision)};wqn::NotePackSyncResult out;
    const auto result=bulk?wqn::SyncNotePacks("synthetic-token",&out):wqn::SyncSingleNotebookPack("synthetic-token",notebook,&out);
    Check(result==ESP_OK && out.index_ready,"real installer produced a complete index");return out;
}
NoteCloudResult Result(bool bulk,wqn::NotePackSyncResult result) {
    NoteCloudResult out;out.op=bulk?NoteCloudOp::kPackSync:NoteCloudOp::kFetchNotebookPack;
    out.result=result.result;out.pack_index_ready=result.index_ready;out.pack_index=std::move(result.index);
    out.message=result.message;out.fetched_notebook_id=notebook;return out;
}
void Deliver(UiState& state,bool bulk,NoteCloudResult& result) {
    const auto domain=bulk?CloudDomain::kNoteBulk:CloudDomain::kNote;
    uint32_t generation=0;
    Check(TakeCloudResultToApply(domain,&generation) && generation==1,"mailbox exposes the producer-owned result");
    bool changed=false;(void)ApplyNoteCloudResult(&state,result,&changed);AckCloudResult(domain,generation);
    Check(!HasCloudResultPending(domain),"stale or current result is acknowledged without wedging its lane");
}
bool Version(const wqn::NotePackIndex& index,unsigned expected)
{return index.notebooks.size()==1 && index.notebooks[0].title=="v"+std::to_string(expected);}
void ArmOpen(UiState& state) {
    state.note_app.mode=wqn::NoteAppMode::kNoteList;state.note_app.session.persisted.active=true;
    state.note_app.body_fetch_notebook_id=notebook;state.note_app.body_fetch_pending_open=true;
    state.note_app.body_fetch_in_flight=true;state.note_app.body_fetch_pending_item_id=std::string(36,'b');
    wqn::StoredNoteSessionItem item;std::strcpy(item.item_id,state.note_app.body_fetch_pending_item_id.c_str());
    std::strcpy(item.notebook_id,notebook.c_str());state.note_app.session.persisted.remote.items.push_back(item);
}
void Cases() {
    for(bool older_bulk:{false,true}) for(bool newer_first:{false,true}) for(bool browsing:{false,true}) {
        Reset();auto a=Build(older_bulk,1),b=Build(!older_bulk,2);
        Check(Generation(a.index)>0 && Generation(b.index)>Generation(a.index),"both installers stamp one shared build order");
        auto first=Result(older_bulk,std::move(a)),second=Result(!older_bulk,std::move(b));
        PublishCloudResult(CloudDomain::kNote,1);PublishCloudResult(CloudDomain::kNoteBulk,1);
        UiState state;state.note_app.mode=browsing?wqn::NoteAppMode::kNoteList:wqn::NoteAppMode::kNotebookList;
        state.note_app.session.persisted.active=browsing;state.note_app.body_fetch_notebook_id=notebook;
        if(newer_first) {Deliver(state,!older_bulk,second);Deliver(state,older_bulk,first);}
        else {Deliver(state,older_bulk,first);Deliver(state,!older_bulk,second);}
        Check(!state.note_app.pending_pack_index_ready || Version(state.note_app.pending_pack_index,2),
              "stale bulk delivery cannot replace or re-arm an older pending aggregate");
        state.note_app.mode=wqn::NoteAppMode::kNotebookList;state.note_app.session.persisted.paused=true;
        ActivatePendingNotePackIndex(&state.note_app);
        Check(Version(state.note_app.pack_index,2),"newest complete index survives delivery reversal and pending activation");
        Check(!state.note_app.pending_pack_index_ready,"installed newest index retires obsolete pending payload");
        Check(stored.notebooks[0].content_revision==2,"UI scheduling never rewrites the durable manifest");
    }
    // The direct pending -> targeted -> return path also fails without a
    // reversed mailbox: an old staged payload must not remain armed.
    Reset();auto a=Build(true,3),b=Build(false,4);UiState state;
    state.note_app.mode=wqn::NoteAppMode::kNoteList;state.note_app.session.persisted.active=true;
    auto older=Result(true,std::move(a)),newer=Result(false,std::move(b));
    bool changed=false;ApplyNoteCloudResult(&state,older,&changed);ApplyNoteCloudResult(&state,newer,&changed);
    Check(Version(state.note_app.pack_index,4),"targeted success installs its newer aggregate while browsing");
    Check(!state.note_app.pending_pack_index_ready,"targeted success disarms an older staged aggregate");
    state.note_app.mode=wqn::NoteAppMode::kNotebookList;state.note_app.session.persisted.paused=true;
    ActivatePendingNotePackIndex(&state.note_app);Check(Version(state.note_app.pack_index,4),"return cannot reactivate an obsolete staged index");

    Reset();auto target=Build(false,5),bulk=Build(true,6);state={};ArmOpen(state);
    auto target_result=Result(false,std::move(target)),bulk_result=Result(true,std::move(bulk));
    ApplyNoteCloudResult(&state,bulk_result,&changed);
    Check(state.note_app.pending_pack_index_ready && body_reads==0,"bulk stages without reading an active browse body");
    ApplyNoteCloudResult(&state,target_result,&changed);
    Check(Version(state.note_app.pack_index,5) && body_reads==1 && body_revision==5 &&
          state.note_app.mode==wqn::NoteAppMode::kNoteView && !state.note_app.body_fetch_pending_open,
          "targeted completion honors its open without prematurely activating the newer full catalog");
    Check(state.note_app.pending_pack_index_ready && Version(state.note_app.pending_pack_index,6),
          "newer full aggregate stays staged for the return to the notebook list");
    state.note_app.mode=wqn::NoteAppMode::kNotebookList;state.note_app.session.persisted.paused=true;
    ActivatePendingNotePackIndex(&state.note_app);
    Check(Version(state.note_app.pack_index,6) && !state.note_app.pending_pack_index_ready,
          "return activates the newer staged catalog after the targeted body was opened");
    Reset();auto old=Build(false,1),latest=Build(true,2);state={};
    auto old_result=Result(false,std::move(old)),latest_result=Result(true,std::move(latest));
    ApplyNoteCloudResult(&state,latest_result,&changed);ArmOpen(state);
    ApplyNoteCloudResult(&state,old_result,&changed);
    Check(Version(state.note_app.pack_index,2) && body_revision==2 && state.note_app.current_note_loaded,
          "late targeted callback uses an already newer active aggregate without replacing it");
    const auto active=state.note_app.pack_index;
    state.note_app.body_fetch_notebook_id=std::string(36,'c');state.note_app.body_fetch_pending_open=true;
    state.note_app.body_fetch_request=true;const auto reads=body_reads;
    wqn::ApplyNoteBodyFetchResult(&state.note_app,ESP_OK,active,true,false,notebook,"old-notebook");
    Check(state.note_app.body_fetch_pending_open && state.note_app.body_fetch_request && body_reads==reads,
          "generation filtering preserves the other notebook's pending open intent");

    Reset();auto prior=Build(true,9),recreated=Build(false,1);state={};
    auto recreated_result=Result(false,std::move(recreated)),prior_result=Result(true,std::move(prior));
    ApplyNoteCloudResult(&state,recreated_result,&changed);ApplyNoteCloudResult(&state,prior_result,&changed);
    Check(Version(state.note_app.pack_index,1),"local install order permits lower remote revisions after recreation at the same cursor");
    Reset();auto removed=Build(false,1);remote.notebooks.clear();wqn::NotePackSyncResult empty;
    Check(wqn::SyncNotePacks("synthetic-token",&empty)==ESP_OK && empty.index_ready,"full installer builds a newer empty catalog");
    state={};auto empty_result=Result(true,std::move(empty)),removed_result=Result(false,std::move(removed));
    ApplyNoteCloudResult(&state,empty_result,&changed);ApplyNoteCloudResult(&state,removed_result,&changed);
    Check(state.note_app.pack_index.notebooks.empty(),"old single-notebook delivery cannot resurrect a deleted catalog row");

    Reset();auto installed=Build(true,1);state={};auto installed_result=Result(true,installed);
    ApplyNoteCloudResult(&state,installed_result,&changed);
    wqn::NotePackSyncResult unchanged;
    Check(wqn::SyncNotePacks("synthetic-token",&unchanged)==ESP_OK && !unchanged.index_ready && Generation(unchanged.index)==0,
          "no-change result does not invent a materialized index generation");
    auto no_index=Result(true,std::move(unchanged));ApplyNoteCloudResult(&state,no_index,&changed);
    Check(Version(state.note_app.pack_index,1),"status-only success retains the active index");
    wqn::NotePackIndex initial;wqn::ApplyNotePackIndex(&state.note_app,std::move(initial),"late-initial-load");
    Check(Version(state.note_app.pack_index,1),"unversioned local load cannot replace a versioned installed index");
    state.note_app.pending_pack_index=installed.index;state.note_app.pending_pack_index_ready=true;
#if HAS_GENERATION
    state.note_app.pending_pack_index.delivery_generation=Generation(state.note_app.pack_index)-1;
#endif
    state.note_app.pending_pack_index.notebooks.clear();
    ActivatePendingNotePackIndex(&state.note_app);
    Check(Version(state.note_app.pack_index,1) && !state.note_app.pending_pack_index_ready,
          "fault-injected obsolete pending activation cannot replace the active index");

    Reset();remote.notebooks={Row(1)};fail_index=1;wqn::NotePackSyncResult failed;
    Check(wqn::SyncNotePacks("synthetic-token",&failed)!=ESP_OK && !failed.index_ready && Generation(failed.index)==0,
          "failed rebuild publishes no accepted generation");
    fail_index=0;auto recovered=Build(false,2);
    Check(Generation(recovered.index)>0,"other installer resumes after failed rebuild");
    Reset();(void)Build(true,1);remote.notebooks={Row(2)};fail_index=1;
    wqn::NotePackSyncResult targeted_failure;
    Check(wqn::SyncSingleNotebookPack("synthetic-token",notebook,&targeted_failure)!=ESP_OK &&
          !targeted_failure.index_ready,"targeted commit can precede a failed index rebuild");
    fail_index=0;wqn::NotePackSyncResult retry;
    Check(wqn::SyncNotePacks("synthetic-token",&retry)==ESP_OK && retry.index_ready && Version(retry.index,2),
          "unchanged full sync rebuilds after the targeted lane left the index stale");
#if HAS_GENERATION
    const auto previous=wqn::g_note_index_generation.load();
    wqn::g_note_index_generation.store(uint64_t(std::numeric_limits<uint32_t>::max())+10);
    Reset();auto wide=Build(true,1);
    Check(Generation(wide.index)>std::numeric_limits<uint32_t>::max(),"delivery order retains all 64 generation bits");
    wqn::g_note_index_generation.store(std::numeric_limits<uint64_t>::max());
    const auto before_io=io_calls;
    for(bool full:{true,false}) {
        wqn::NotePackSyncResult exhausted;
        const auto result=full?wqn::SyncNotePacks("synthetic-token",&exhausted):
            wqn::SyncSingleNotebookPack("synthetic-token",notebook,&exhausted);
        Check(result!=ESP_OK && !exhausted.index_ready && Generation(exhausted.index)==0 && io_calls==before_io,
              "generation exhaustion remains saturated and refuses unorderable I/O on either lane");
    }
    wqn::g_note_index_generation.store(previous);
#endif
}
int main() {Cases();std::printf("Note index delivery: %zu checks, %zu failures; real installers/apply/mailbox, host ports; NOT HIL\n",checks,failures);return failures?1:0;}
