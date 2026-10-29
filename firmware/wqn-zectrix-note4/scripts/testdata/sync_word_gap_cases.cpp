void ResetWordGap() {
    Reset();
    word_pending.clear(); word_quarantined.clear(); word_parked.clear(); word_errors.clear();
    g_word_outbox_gap_terminal_session_id.clear();
    g_word_outbox_retry_cause = OutboxRetryCause::kNone;
    word_transport_failure = word_retryable = false;
    word_yield_after_quarantine = false;
    word_calls = 0;
    for (unsigned i = 0; i < 4; ++i) {
        wqn::DurableWordObservation entry;
        entry.request_id = "gap-request-" + std::to_string(i);
        entry.session_id = i < 3 ? "retired-session" : "new-session";
        entry.sequence = i + 10;
        entry.item_id = "original-item-" + std::to_string(i);
        entry.occurred_at = "2026-10-29T00:00:00Z";
        word_pending.push_back(entry);
    }
    word_errors["retired-session"] = "SEQUENCE_GAP";
}
void RebootWordGap() {
    // Deserialize the bytes actually saved by production before clearing RAM.
    // The outbox port preserves records; its physical journal has separate tests.
    wqn::SyncJournal decoded;
    Check(ParseSyncJournalPayload(durable_payload, &decoded) == ESP_OK,
          "GAP: persisted checkpoint bytes decode before simulated reboot");
    durable_journal = decoded;
    g_sync_journal = {}; g_sync_journal_loaded = false;
    g_sync_journal_durable_payload_valid = false;
    g_word_outbox_retry_request_id.clear(); g_word_outbox_retry_attempts = 0;
    g_word_outbox_retry_not_before_ms = 0; g_word_outbox_retry_cause = OutboxRetryCause::kNone;
    g_word_outbox_gap_terminal_session_id.clear();
    mock_wall += 600; mock_ms = 0;
    Check(EnsureSyncJournalLoaded() == ESP_OK,
          "GAP: real startup restores checkpoint after simulated reboot");
}
void RunWordGapCases() {
    using S = WordOutboxUploadState;
    for (unsigned reboot_after = 1; reboot_after <= 5; ++reboot_after) {
        ResetWordGap();
        for (unsigned i = 1; i <= 5; ++i) {
            Check(UploadPendingWordObservations("host-placeholder") == S::kPending &&
                  g_word_outbox_retry_attempts == i && word_pending.size() == 4 &&
                  word_quarantined.empty() && word_parked.empty() && !g_outbox_protocol_suspended,
                  "GAP: genuine SEQUENCE_GAP retries without early quarantine or suspension");
            wqn::SyncJournal checkpoint;
            Check(ParseSyncJournalPayload(durable_payload, &checkpoint) == ESP_OK &&
                  checkpoint.word_outbox.attempt == i &&
                  std::string(checkpoint.word_outbox.request_id) == "gap-request-0",
                  "GAP: each retry identity and attempt survive production serialization");
            const int calls = word_calls;
            Check(UploadPendingWordObservations("host-placeholder") == S::kPending && word_calls == calls,
                  "GAP: before deadline neither HTTP nor retry count advances");
            if (i == reboot_after) {
                RebootWordGap();
                Check(g_word_outbox_retry_attempts == i &&
                      g_word_outbox_retry_request_id == "gap-request-0",
                      "GAP: reboot retains threshold progress and original identity");
            }
            mock_ms = g_word_outbox_retry_not_before_ms;
        }
        word_errors["new-session"] = "SEQUENCE_GAP";
        const int calls = word_calls;
        Check(UploadPendingWordObservations("host-placeholder") == S::kPending &&
              word_quarantined.size() == 3 && word_parked.empty() && word_pending.size() == 1 &&
              word_calls == calls + 4 && g_word_outbox_retry_attempts == 1 &&
              word_pending.front().session_id == "new-session" && !g_outbox_protocol_suspended,
              "GAP: threshold retires same-session backlog while a new session starts its own ladder");
        if (word_quarantined.size() == 3) {
            for (unsigned i = 0; i < 3; ++i) {
                Check(word_quarantined[i].request_id == "gap-request-" + std::to_string(i) &&
                      word_quarantined[i].item_id == "original-item-" + std::to_string(i) &&
                      word_quarantined[i].sequence == i + 10,
                      "GAP: quarantine receives each original payload in FIFO order");
            }
        }
        word_errors.clear(); mock_ms = g_word_outbox_retry_not_before_ms;
        Check(UploadPendingWordObservations("host-placeholder") == S::kDrained &&
              word_pending.empty() && g_word_outbox_gap_terminal_session_id.empty(),
              "GAP: healthy session uploads and queue drain clears the retired-session hint");
    }
    ResetWordGap();
    UploadPendingWordObservations("host-placeholder");
    word_errors.clear(); mock_ms = g_word_outbox_retry_not_before_ms;
    Check(UploadPendingWordObservations("host-placeholder") == S::kDrained &&
          word_quarantined.empty() && word_parked.empty(),
          "GAP: a transient gap that resolves before threshold loses no record");
    for (const bool transport : {false, true}) {
        ResetWordGap(); word_transport_failure = transport;
        word_errors["retired-session"] = transport ? "SEQUENCE_GAP" : "SERVER_BUSY";
        word_retryable = !transport;
        for (int i = 0; i < 7; ++i) {
            Check(UploadPendingWordObservations("host-placeholder") == S::kPending &&
                  word_pending.size() == 4 && word_quarantined.empty() && word_parked.empty() &&
                  g_word_outbox_gap_terminal_session_id.empty() && !g_outbox_protocol_suspended,
                  "GAP: transport ambiguity and retryable server errors never retire or suspend sessions");
            mock_ms = g_word_outbox_retry_not_before_ms;
        }
    }
    ResetWordGap();
    for (int i = 0; i < 5; ++i) {
        UploadPendingWordObservations("host-placeholder");
        mock_ms = g_word_outbox_retry_not_before_ms;
    }
    word_yield_after_quarantine = true;
    Check(UploadPendingWordObservations("host-placeholder") == S::kYielded &&
          word_quarantined.size() == 1 && word_pending.size() == 3,
          "GAP: interaction can stop a round immediately after retiring its head");
    RebootWordGap();
    Check(g_word_outbox_gap_terminal_session_id.empty() && g_word_outbox_retry_attempts == 0,
          "GAP: reboot does not invent a durable retired-session hint");
    const int before = word_calls;
    for (int rounds = 0; rounds < 15 && !word_pending.empty(); ++rounds) {
        UploadPendingWordObservations("host-placeholder");
        mock_ms = g_word_outbox_retry_not_before_ms;
    }
    Check(word_pending.empty() && word_quarantined.size() == 3 && word_parked.empty() &&
          word_calls <= before + 8 && !g_outbox_protocol_suspended,
          "GAP: reboot after head quarantine still drains remaining records with a bounded fresh ladder");
}
