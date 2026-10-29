// Included after the real codec, rotation and libc interruption ports.
void JournalIdentityCases(const std::string& old_payload,const std::string& new_payload,
                          const SyncJournal& old_state,const SyncJournal& new_state)
{
    std::vector<std::string> cases;
    const auto insert=[&](const char* needle,size_t offset,const char* suffix) {
        auto text=new_payload;const auto pos=text.find(needle);if(pos==std::string::npos) std::abort();
        text.insert(pos+offset,suffix);cases.push_back(text);
    };
    insert("\"sync_cursor\"",std::strlen("\"sync_cursor"),R"(\u0000shadow)");
    insert("\"sync_cursor\"",std::strlen("\"sync_cursor"),R"(\uZZZZshadow)");
    insert("\"protocol_blocked_image_id\":\"",std::strlen("\"protocol_blocked_image_id\":\""),R"(image\u0000shadow)");
    insert("\"protocol_blocked_image_id\":\"",std::strlen("\"protocol_blocked_image_id\":\""),R"(image\u0z00shadow)");
    insert("\"desired_snapshot_id\":\"",std::strlen("\"desired_snapshot_id\":\""),
           (std::string(64,'a')+R"(\u0000shadow)").c_str());
    insert("\"request_id\":\"",std::strlen("\"request_id\":\""),R"(replay-id\u0000shadow)");
    cases.push_back(new_payload.substr(0,new_payload.size()-1)+R"(,"extra":"x\u0000y"})");
    cases.push_back(new_payload.substr(0,new_payload.size()-1)+R"(,"extra":"x\uxxxxy"})");
    for(const auto& bad:cases) {
        SyncJournal loaded;
        Reset();WriteHost(primary,bad);WriteHost(backup,old_payload);WriteHost(temp,"staging-sentinel");
        loaded.sync_cursor=999;
        Check(wqn::LoadSyncJournal(&loaded)==ESP_ERR_NOT_SUPPORTED && loaded.sync_cursor==0,
              "opaque primary cannot publish a prefix cursor/identity or fall back to an older checkpoint");
        Check(wqn::SaveSyncJournalThroughStorageService(new_state,"opaque-primary")==ESP_ERR_NOT_SUPPORTED &&
              metadata_ops==0 && ReadHost(primary)==bad && ReadHost(backup)==old_payload && ReadHost(temp)=="staging-sentinel",
              "unsupported primary is retained before any staging or rotation mutation");
        Reset();WriteHost(primary,new_payload);WriteHost(backup,bad);WriteHost(temp,"staging-sentinel");
        Check(wqn::LoadSyncJournal(&loaded)==ESP_OK && loaded.sync_cursor==new_state.sync_cursor,
              "a supported current primary remains readable with an opaque older backup");
        Check(wqn::SaveSyncJournalThroughStorageService(old_state,"opaque-backup")==ESP_ERR_NOT_SUPPORTED &&
              metadata_ops==0 && ReadHost(primary)==new_payload && ReadHost(backup)==bad && ReadHost(temp)=="staging-sentinel",
              "replacement cannot unlink an opaque backup as a side effect");
        Reset();WriteHost(backup,bad);WriteHost(temp,"staging-sentinel");
        Check(wqn::LoadSyncJournal(&loaded)==ESP_ERR_NOT_SUPPORTED && loaded.sync_cursor==0,
              "an opaque sole backup is an explicit recovery error, never empty progress");
        Check(wqn::SaveSyncJournalThroughStorageService(new_state,"opaque-only-backup")==ESP_ERR_NOT_SUPPORTED &&
              metadata_ops==0 && ::access(kSyncJournalPath,F_OK)!=0 && ReadHost(backup)==bad && ReadHost(temp)=="staging-sentinel",
              "absent primary cannot authorize overwrite of the opaque backup");
        // Restore a supported checkpoint as an explicit recovery action.
        WriteHost(backup,old_payload);
        Check(wqn::SaveSyncJournalThroughStorageService(new_state,"after-recovery")==ESP_OK &&
              wqn::LoadSyncJournal(&loaded)==ESP_OK && loaded.sync_cursor==new_state.sync_cursor,
              "a refused opaque save does not poison retry after source recovery");
    }
    Reset();
    const auto literal=new_payload.substr(0,new_payload.size()-1)+R"(,"extra":"literal\\u0000 中文"})";
    WriteHost(primary,literal);SyncJournal loaded;
    Check(wqn::LoadSyncJournal(&loaded)==ESP_OK && loaded.sync_cursor==new_state.sync_cursor,
          "literal Unicode-escape text remains representable in a checkpoint");
    // Count real primary decode allocations, then fail each one. The writer
    // must not mistake an unreadable valid checkpoint for disposable corruption.
    allocation_calls=allocation_live=allocation_fail_at=0;
    cJSON_Hooks hooks{JournalAllocate,JournalFree};cJSON_InitHooks(&hooks);
    Check(ParseSyncJournalPayload(new_payload,&loaded)==ESP_OK,"journal decode allocation baseline succeeds");
    const auto sites=allocation_calls;cJSON_InitHooks(nullptr);
    Check(sites>20 && allocation_live==0,"real decode allocation baseline frees all JSON nodes");
    for(int action=0;action<2;++action) for(int point=1;point<=sites;++point) {
        Reset();WriteHost(primary,new_payload);WriteHost(backup,old_payload);WriteHost(temp,"staging-sentinel");
        allocation_calls=allocation_live=0;allocation_fail_at=point;cJSON_InitHooks(&hooks);
        const auto result=action==0?wqn::LoadSyncJournal(&loaded):WriteSyncJournalFileAtomic(old_payload);
        cJSON_InitHooks(nullptr);allocation_fail_at=0;
        Check(result==ESP_ERR_NO_MEM && allocation_live==0 && metadata_ops==0 &&
              ReadHost(primary)==new_payload && ReadHost(backup)==old_payload && ReadHost(temp)=="staging-sentinel",
              "decode OOM propagates and preserves both durable generations before any mutation");
        Check(wqn::LoadSyncJournal(&loaded)==ESP_OK && loaded.sync_cursor==new_state.sync_cursor,
              "decode retry after memory recovery reads the latest supported checkpoint");
    }
    std::printf("Journal identity: %zu aliases; %d primary decode sites x load/write tested\n",cases.size(),sites);
    Reset();WriteHost(primary,new_payload);WriteHost(backup,old_payload);
}
