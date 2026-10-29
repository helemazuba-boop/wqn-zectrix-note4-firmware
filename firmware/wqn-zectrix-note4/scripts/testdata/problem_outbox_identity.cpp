// Included by the real-codec recovery harness after its libc fault ports.
static void IdentityPreservationCases()
{
    const auto a=Value("opaque-A"),b=Value("opaque-B"),c=Value("opaque-C");
    const auto good=EncodeObservationLine(a);
    std::vector<std::string> opaque;
    const auto alter=[&](const std::string& needle,size_t offset,const char* escape) {
        auto wire=good;const auto pos=wire.find(needle);
        if(pos==std::string::npos) std::abort();
        wire.insert(pos+offset,escape);opaque.push_back(wire);
    };
    alter(a.request_id,a.request_id.size(),R"(\u0000shadow)");
    alter("\"request_id\"",std::strlen("\"request_id"),R"(\u0000shadow)");
    alter(a.problem_id,a.problem_id.size(),R"(\u0000shadow)");
    alter(a.occurred_at,a.occurred_at.size(),R"(\u0000shadow)");
    alter("skip",4,R"(\u0000shadow)");
    alter(a.request_id,a.request_id.size(),R"(\uZZZZshadow)");
    alter("\"request_id\"",std::strlen("\"request_id"),R"(\u0z00shadow)");
    opaque.push_back(good.substr(0,good.size()-1)+R"(,"extra":"x\u0000y"})");
    opaque.push_back(good.substr(0,good.size()-1)+R"(,"extra":"x\uxxxxy"})");
    auto maximum=opaque.back();maximum.pop_back();maximum+=",\"padding\":\"";
    maximum.append(512-maximum.size()-2,'p');maximum+="\"}";
    opaque.push_back(maximum);

    for(const auto& line:opaque) for(const std::string frame:{"\n","\r\n"}) {
        Reset();Observation parsed,head;
        Check(DecodeObservationLine(line,&parsed)!=ObservationDecode::kValid && parsed.request_id.empty(),
              "opaque decoder publishes no truncated request identity");
        const auto held=line+frame;
        RawPut(held+EncodeObservationLine(b)+'\n');
        Check(Peek(&head)==ESP_OK && head.request_id==b.request_id,
              "opaque head cannot wedge later uploadable verdicts");
        wqn::ProblemOutboxSnapshot snapshot;
        Check(Snapshot(&snapshot)==ESP_OK && snapshot.pending_count==1 && snapshot.suspended_count==1,
              "opaque records consume one visible suspended/capacity slot");
        Check(Remove(a)==ESP_OK && Remove(a,true)==ESP_OK &&
              FileBytes(kOutboxPath).find(held)!=std::string::npos && FileBytes(kRejectedPath).empty(),
              "ACK and quarantine for a prefix cannot delete or falsely reject the opaque record");
        Check(Suspend(a)==ESP_ERR_NOT_FOUND && FileBytes(kParkedPath).empty(),
              "opaque identity is never bound to a prefix park marker");
        Check(Commit(a)==ESP_OK && Commit(a)==ESP_OK && Snapshot(&snapshot)==ESP_OK &&
              snapshot.pending_count==2 && snapshot.suspended_count==1,
              "a distinct valid prefix record appends idempotently while opaque bytes remain");
        Check(Suspend(b)==ESP_OK && Peek(&head)==ESP_OK && head.request_id==a.request_id,
              "normal parking and queue progress work around an opaque record");
        Check(Remove(a)==ESP_OK && Remove(b,true)==ESP_OK && FileBytes(kOutboxPath)==held,
              "all valid ACKs retain opaque payload and original LF/CRLF framing exactly");
        Check(Snapshot(&snapshot)==ESP_OK && snapshot.pending_count==0 && snapshot.suspended_count==1 &&
              Peek(&head)==ESP_ERR_NOT_FOUND && !g_empty_problem_outbox_known,
              "opaque-only journal is suspended, never measured empty or uploadable");
        // Query/marker RAM state is disposable; the opaque bytes are the state.
        g_empty_problem_outbox_known=false;
        Check(Commit(c)==ESP_OK && Peek(&head)==ESP_OK && head.request_id==c.request_id,
              "post-restart normal append retains opaque data and restores progress");
        const auto before=FileBytes(kOutboxPath);
        fail_publish=fail_restore=true;
        Check(Remove(c)==ESP_FAIL,"failed ACK publication does not report success");
        fail_publish=fail_restore=false;g_empty_problem_outbox_known=false;
        Check(Peek(&head)==ESP_OK && head.request_id==c.request_id && FileBytes(kOutboxPath)==before,
              "interrupted publication recovers opaque and valid records from backup");
        partial_write_path=kOutboxTempPath;
        Check(Remove(c)==ESP_FAIL && FileBytes(kOutboxPath)==before,
              "short rewrite preserves the original opaque generation");
        partial_write_path.clear();bad_close_path=kOutboxTempPath;
        Check(Remove(c)==ESP_FAIL && FileBytes(kOutboxPath)==before,
              "failed rewrite close preserves the original opaque generation");
        bad_close_path.clear();
        RawPut(before+"{bad}\n{torn");
        Check(Commit(a)==ESP_OK && FileBytes(kOutboxPath).find(held)!=std::string::npos &&
              FileBytes(kOutboxPath).find("{bad}")==std::string::npos,
              "bounded corrupt-frame repair retains opaque records through append rewrite");
    }
    Reset();std::string full;
    for(unsigned i=0;i<wqn::kProblemObservationOutboxCapacity;++i) full+=opaque.front()+'\n';
    RawPut(full);wqn::ProblemOutboxSnapshot snapshot;
    Check(Snapshot(&snapshot)==ESP_OK && snapshot.pending_count==0 && snapshot.suspended_count==200 &&
          Commit(a)==ESP_ERR_NO_MEM && FileBytes(kOutboxPath)==full,
          "opaque records remain bounded by the same durable capacity, with no overwrite");
    Reset();
    const auto literal=good.substr(0,good.size()-1)+R"(,"extra":"literal\\u0000 中文"})";
    RawPut(literal+'\n');Observation parsed;
    Check(Peek(&parsed)==ESP_OK && parsed.request_id==a.request_id && Snapshot(&snapshot)==ESP_OK &&
          snapshot.suspended_count==0,"literal escaped backslash and Unicode remain uploadable");
    auto bad_input=a;bad_input.occurred_at+='\0';bad_input.occurred_at+="suffix";
    Check(!IsValidObservation(bad_input),"producer validation rejects timestamps that C-string encoding would truncate");
    Reset();
    std::printf("Opaque identity: %zu variants x LF/CRLF and capacity/legacy/producer cases exercised\n",opaque.size());
}
