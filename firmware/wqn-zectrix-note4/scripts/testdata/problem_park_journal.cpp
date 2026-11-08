// Owner transactions on synthetic libc files; no real device or HTTP access.
void ParkOpenHook(const char* path,const char* mode) {
    if(std::strcmp(path,kParkedPath)!=0)return;
    if(std::strcmp(mode,"rb")==0)++park_read_opens;
    if(std::strchr(mode,'+')) {
        ++park_update_opens;
        if(park_remove_before_update) {park_remove_before_update=false;__real_remove(path);}
        if(park_change_before_update) {park_change_before_update=false;ProofPut(path,park_drift_bytes);}
    }
}
void ParkSyncHook() {
    if(park_stream_update && park_change_during_sync) {
        park_change_during_sync=false;ProofPut(kParkedPath,park_drift_bytes);
    }
}
std::string ParkLine(const Observation& value,const char* ending="\n") {
    return value.request_id+" actor-ownership"+ending;
}
void ParkJournalCases() {
    const int old_pass=passes,old_fail=failures;
    const auto a=Value("park-A"),b=Value("park-B"),c=Value("park-C");
    const auto line=ParkLine(a);
    const std::vector<std::string> bad={"broken\n",a.request_id+" actor-ownershi\n",a.request_id+" future-reason\n",
        a.request_id+" actor-ownership extra\n",a.request_id+std::string("\0hidden",7)+" actor-ownership\n",
        "\xff"+line,a.request_id.substr(0,8)+'\r'+a.request_id.substr(8)+" actor-ownership\n",
        a.request_id+" actor-\rownership\n","\r"+line,"\n","\r\n","   \n","\t\n",ParkLine(a,"\r"),std::string(120,'x')+'\n'};
    for(const auto& bytes:bad) {
        Reset();Commit(a);Commit(b);ProofPut(kParkedPath,bytes);const auto payload=ProofBytes(kOutboxPath);
        const int writes=write_opens;std::vector<std::string> ids={"untrusted"},rows={"untrusted"};Observation out;wqn::ProblemOutboxSnapshot snapshot;
        Check(ReadParkedRequestIds(&ids)!=ESP_OK && ids.empty(),"park: malformed marker never publishes partial IDs");
        Check(Peek(&out)!=ESP_OK && out.request_id.empty(),"park: malformed marker cannot become an uploadable head");
        Check(Snapshot(&snapshot)!=ESP_OK,"park: malformed marker does not become a successful empty/suspended snapshot");
        Check(ReadOutboxForQuery(&rows,&ids)!=ESP_OK && rows.empty() && ids.empty(),"park: query failure clears both result vectors");
        Check(Remove(a)!=ESP_OK && ProofBytes(kOutboxPath)==payload,"park: ACK revalidates marker state before deleting payload");
        Check(Remove(a,true)!=ESP_OK && ProofBytes(kOutboxPath)==payload && !ProofExists(kRejectedPath),"park: quarantine cannot mutate an ambiguous marker state");
        Check(Suspend(b)!=ESP_OK && ProofBytes(kParkedPath)==bytes,"park: malformed or torn tail is never extended by another marker");
        Check(write_opens==writes && ProofBytes(kOutboxPath)==payload,"park: invalid metadata leaves all source bytes untouched");
        Check(Commit(c)==ESP_OK && ProofBytes(kParkedPath)==bytes && Peek(&out)!=ESP_OK,"park: new learning remains durable while upload admission is held");
    }
    for(const char* ending:{"\n","\r\n",""}) {
        Reset();Commit(a);Commit(b);ProofPut(kParkedPath,ParkLine(a,ending));Observation out;wqn::ProblemOutboxSnapshot snapshot;
        Check(Peek(&out)==ESP_OK && out.request_id==b.request_id,"park: valid LF CRLF and complete legacy EOF preserve queue progress");
        Check(Snapshot(&snapshot)==ESP_OK && snapshot.pending_count==1 && snapshot.suspended_count==1,"park: valid legacy framing retains suspended counts");
        Check(Suspend(b)==ESP_OK && Peek(&out)==ESP_ERR_NOT_FOUND,"park: append separates a complete EOF record without merging identities");
    }
    Reset();Commit(a);std::string duplicate;
    for(unsigned i=0;i<200;++i)duplicate+=line;
    ProofPut(kParkedPath,duplicate);std::vector<std::string> ids;
    Check(ReadParkedRequestIds(&ids)==ESP_OK && ids.size()==1,"park: bounded valid duplicate markers remain compatible");
    Check(AppendParkedEntry(b.request_id,wqn::OutboxSuspendReason::kActorOwnership)!=ESP_OK &&
        ProofBytes(kParkedPath)==duplicate,"park: append capacity counts physical records rather than distinct identities");
    ProofPut(kParkedPath,duplicate+line);
    Check(ReadParkedRequestIds(&ids)!=ESP_OK && ids.empty(),"park: physical record limit cannot be bypassed by duplicate IDs");
    for(const char* tail:{"broken\n","broken"}) {
        Reset();ProofPut(kParkedPath,line+tail);ids={"untrusted"};
        Check(ReadParkedRequestIds(&ids)!=ESP_OK && ids.empty(),"park: a malformed or torn suffix discards even the valid prefix result");
    }
    Reset();ProofPut(kParkedPath,std::string(200*(96+2)+1,'x'));const int opens=read_opens;
    Check(ReadParkedRequestIds(&ids)!=ESP_OK && read_opens==opens,"park: oversized journal is rejected before scanning bytes");
    for(unsigned kind=0;kind<3;++kind) {
        Reset();ProofPut(std::string(kParkedPath)+"-target",line);
        int created=kind==0?symlink((std::string(kParkedPath)+"-target").c_str(),kParkedPath):
            kind==1?mkfifo(kParkedPath,0600):mkdir(kParkedPath,0700);
        if(created!=0)std::abort();
        // Avoid opening a FIFO in the old-source probe; opening it at all is
        // the failed assertion. No timeout is classified as a semantic pass.
        if(kind==1)bad_read_path=kParkedPath;
        const int before=read_opens;
        Check(ReadParkedRequestIds(&ids)!=ESP_OK && read_opens==before,"park: nonregular marker file is rejected before any open");
        __real_remove((std::string(kParkedPath)+"-target").c_str());
    }
    Reset();ProofPut(kParkedPath,ParkLine(a,""));park_fail_read_close_at=2;
    Check(AppendParkedEntry(b.request_id,wqn::OutboxSuspendReason::kActorOwnership)!=ESP_OK &&
        ProofBytes(kParkedPath)==ParkLine(a,"") && write_opens==0,"park: separator probe close failure cannot append bytes");
    Reset();Commit(a);Commit(b);park_fail_sync=true;
    Check(Suspend(a)!=ESP_OK,"park: failed marker synchronization is not acknowledged");
    const auto uncertain=ProofBytes(kParkedPath);Observation out;wqn::ProblemOutboxSnapshot snapshot;
    Check(Peek(&out)!=ESP_OK && Snapshot(&snapshot)!=ESP_OK,"park: valid-looking bytes from a failed write remain unconfirmed");
    Check(Suspend(a)!=ESP_OK && ProofBytes(kParkedPath)==uncertain,"park: idempotent retry cannot bypass the unresolved synchronization");
    park_fail_sync=false;const int synced=park_sync_calls;
    Check(Peek(&out)==ESP_OK && out.request_id==b.request_id && park_sync_calls>synced,"park: recovery confirms existing bytes before releasing the next head");
    Check(ProofBytes(kParkedPath)==uncertain && Suspend(a)==ESP_OK,"park: successful confirmation changes no marker bytes and preserves idempotence");
    for(unsigned fault=0;fault<5;++fault) {
        Reset();Commit(a);Commit(b);park_fail_sync=true;Suspend(a);park_fail_sync=false;
        const auto retained=ProofBytes(kParkedPath);park_drift_bytes=retained;park_drift_bytes[0]='X';
        if(fault==0)park_fail_update_open=true;
        if(fault==1)park_fail_write_close=true;
        if(fault==2)park_remove_before_update=true;
        if(fault==3)park_change_before_update=true;
        if(fault==4)park_change_during_sync=true;
        Check(Peek(&out)!=ESP_OK,"park: confirmation open close missing-file and drift errors hold admission");
        if(fault<2)Check(ProofBytes(kParkedPath)==retained,"park: failed confirmation never truncates the marker journal");
        if(fault==2)Check(!ProofExists(kParkedPath) && Peek(&out)!=ESP_OK,"park: confirmation cannot create a missing marker journal");
        if(fault>2)Check(ProofBytes(kParkedPath)==park_drift_bytes,"park: detected drift is retained without guessing old IDs");
    }
    Reset();Commit(a);park_fail_write_close=true;
    Check(Suspend(a)!=ESP_OK,"park: write-close uncertainty remains a failed park");
    park_fail_write_close=false;const int sync_before=park_sync_calls;
    Check(Peek(&out)==ESP_ERR_NOT_FOUND && park_sync_calls>sync_before,"park: write-close recovery performs a durable confirmation");
    std::printf("Problem park journal: %d checks, %d failures (libc files; NOT HIL)\n",passes-old_pass+failures-old_fail,failures-old_fail);
    Reset();
}
