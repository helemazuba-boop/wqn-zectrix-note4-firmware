std::vector<DisplayRevision> accepted_revisions;
DisplayRevision next_revision=100;
void ProofReset() {
    Reset();force_fail=false;
    accepted_revisions.clear();send_full=duplicate_terminals=unreserved_terminals=max_queue=0;
    before_send=after_receive=before_submission_return={};
    next_revision=100;
}
void Invariant() {
    const auto live=static_cast<size_t>(std::count_if(g_outstanding_display_revisions.begin(),g_outstanding_display_revisions.end(),
        [](DisplayRevision r){return r!=kInvalidDisplayRevision;}));
    const auto upper=static_cast<size_t>(std::count_if(tracking.records.begin(),tracking.records.end(),
        [](const auto& r){return r.occupied;}));
    Check(live==g_outstanding_display_intents && live<=kDisplayResultQueueDepth,"capacity: lower count equals unique reserved slots and stays bounded");
    Check(results.size()<=live,"capacity: queued terminals never outnumber reservations");
    Check(upper==live,"capacity: UI and owner agree after each complete driver action");
    Check(send_full==0,"capacity: no first full-queue send failure under the supported protocol");
    Check(duplicate_terminals==0 && unreserved_terminals==0,"capacity: every terminal is unique and reserved");
    for(const auto& r:results) {
        Check(HasOutstandingRevisionLocked(r.revision),"capacity: queued terminal retains its reservation until receive and ACK");
    }
}
bool Produce(RefreshSchedule schedule=RefreshSchedule::kImmediate) {
    wqn::UiFrame frame;frame.content=++latest_content;
    const auto revision=next_revision++;
    const auto submission=RequestEpdUiRefresh(frame,std::to_string(frame.content),revision,schedule,WaveformRequirement::kAuto);
    if(submission.accepted) {
        accepted_revisions.push_back(revision);
        Check(TrackDisplaySubmission(&tracking,submission,std::to_string(frame.content),wqn::UiScreen::kHome,"12:00",schedule),
              "capacity: accepted work fits the UI ledger");
    }
    Invariant();return submission.accepted;
}
void Drain() {
    RefreshSchedule ignored=RefreshSchedule::kNone;
    DrainDisplayResults(&ui_runtime,&tracking,&ignored);
    Invariant();
}
void Finish() {
    RunOwner();Drain();
    Check(results.empty() && g_outstanding_display_intents==0 && live_leases==0,
          "capacity: eventual consumer progress drains results, both ledgers and display lease");
    std::vector<DisplayRevision> emitted;
    for(const auto& r:all_results)emitted.push_back(r.revision);
    std::sort(emitted.begin(),emitted.end());
    auto accepted=accepted_revisions;std::sort(accepted.begin(),accepted.end());
    Check(emitted==accepted,"capacity: every accepted intent has exactly one terminal after drain");
}
void FillPrimary() {
    for(size_t i=0;i<kDisplayResultQueueDepth;++i)
        Check(Produce(),"capacity: first sixteen coalesced primary intents are admitted");
    Check(results.size()==kDisplayResultQueueDepth-1 && g_refresh_pending,
          "capacity: fifteen superseded results leave one reserved render result");
}
void Cases() {
    g_refresh_mutex=g_refresh_task=g_display_result_queue=nullptr;
    Check(EnsureDisplayPipelineStarted()==ESP_OK && actual_queue_capacity==kDisplayResultQueueDepth &&
          actual_queue_item_size==sizeof(DisplayResult),
          "capacity: real startup creates a result queue matching the reservation bound and item type");
    ProofReset();FillPrimary();
    const auto before=all_results.size();const auto content=g_pending_frames[g_consumer_index].content;
    Check(!Produce() && all_results.size()==before && g_pending_frames[g_consumer_index].content==content,
          "capacity: seventeenth admission is rejected before replacing pending pixels or publishing");
    RunOwner();Invariant();
    Check(results.size()==kDisplayResultQueueDepth && max_queue==kDisplayResultQueueDepth,
          "capacity: final owner terminal uses the sixteenth slot without dropping it");
    Check(!Produce(),"capacity: a full undrained queue retains all reservations and refuses new work");
    Finish();

    ProofReset();Produce();
    render_hook_once=true;render_hook=[] {
        for(size_t i=1;i<kDisplayResultQueueDepth;++i)Produce(RefreshSchedule::kSelection);
        Check(!Produce(),"capacity: render-time secondary replacement obeys the same admission bound");
    };
    RunOwner();Invariant();
    Check(render_times.size()==2 && results.size()==kDisplayResultQueueDepth,
          "capacity: one primary plus the newest secondary produce sixteen unique terminals");
    Finish();

    ProofReset();Produce();
    before_send=[] {
        for(size_t i=1;i<kDisplayResultQueueDepth;++i)Produce();
        Check(!Produce(),"capacity: producer cannot steal the owner terminal's reservation before queue publication");
    };
    RunOwner();Invariant();Finish();

    ProofReset();FillPrimary();
    after_receive=[] {
        Check(results.size()==kDisplayResultQueueDepth-2 && g_outstanding_display_intents==kDisplayResultQueueDepth,
              "capacity: receive alone frees queue space but does not release admission");
        RunOwner();
        Check(g_outstanding_display_intents==kDisplayResultQueueDepth && send_full==0,
              "capacity: owner can publish while UI holds a received terminal before ACK");
    };
    Drain();Finish();

    ProofReset();
    before_submission_return=[] {
        RunOwner();
        Check(results.size()==1 && tracking.submitted_revision==kInvalidDisplayRevision,
              "capacity: owner may enqueue terminal before producer returns to UI registration");
    };
    Produce();Drain();Finish();

    ProofReset();FillPrimary();force_fail=true;
    RunOwner();Invariant();
    Check(results.size()==kDisplayResultQueueDepth && results.back().status==DisplayStatus::kFailed,
          "capacity: returned render failure occupies its one reserved terminal slot");
    Finish();

    // Reproducible API-level schedules with explicit UI stalls. These permit
    // more producer calls per drain than the normal single UI loop does.
    uint32_t random=0x91a7c42u;
    for(unsigned trial=0;trial<80;++trial) {
        ProofReset();
        for(unsigned step=0;step<64;++step) {
            random^=random<<13;random^=random>>17;random^=random<<5;
            switch(random%5) {
                case 0:case 1:case 2:Produce();break;
                case 3:RunOwner();Invariant();break;
                default:Drain();break;
            }
        }
        Finish();
    }
}
int main() {
    Cases();
    std::printf("Display result capacity: %zu checks, %zu failures at %d Hz; bounded queue, real submit/owner/UI/ACK; NOT HIL\n",
                checks,failures,configTICK_RATE_HZ);
    return failures?1:0;
}
