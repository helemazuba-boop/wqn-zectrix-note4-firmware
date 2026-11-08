// Synthetic libc media and actual production transactions. Never device data.
#include <sys/wait.h>
#include <iterator>
std::string proof_drift_bytes;
std::string ProofArchive(unsigned slot=0) {
    return std::string(kOutboxPath)+".raw"+std::to_string(slot);
}
void ProofPut(const std::string& path,const std::string& bytes) {
    FILE* file=__real_fopen(path.c_str(),"wb");
    if(!file || __real_fwrite(bytes.data(),1,bytes.size(),file)!=bytes.size() || __real_fclose(file)!=0) std::abort();
}
std::string ProofBytes(const std::string& path) {
    std::ifstream input(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
}
bool ProofExists(const std::string& path) { struct stat info{}; return ::lstat(path.c_str(),&info)==0; }
void ProofOpenHook(const char* path,const char* mode) {
    if(std::strcmp(path,kOutboxPath)==0 && std::strcmp(mode,"rb")==0) {
        ++proof_source_opens;
        if(proof_source_opens==2 && proof_copy_read_error) bad_read_path=kOutboxPath;
        if(proof_source_opens==2 && proof_copy_close_error) bad_close_path=kOutboxPath;
        if(proof_drift_copy && proof_source_opens==2) {
            proof_drift_copy=false; ProofPut(kOutboxPath,proof_drift_bytes);
        }
    }
    if(proof_race_stage && std::string(path)==ProofArchive()+".tmp" && std::strchr(mode,'w')) {
        proof_race_stage=false; ProofPut(path,std::string("FOREIGN STAGING BYTES\0",22));
    }
    if(std::strcmp(path,kOutboxTempPath)==0 && std::strchr(mode,'w')) {
        if(proof_drift_publish) { proof_drift_publish=false; ProofPut(kOutboxPath,proof_drift_bytes); }
        if(proof_corrupt_saved) { proof_corrupt_saved=false; ProofPut(ProofArchive(),"changed saved bytes"); }
    }
}
void EvidenceCases() {
    const int start_passes=passes,start_failures=failures;
    const auto a=Value("A"),b=Value("B"),c=Value("C"); Observation out;
    const std::string raw=std::string("bad\0\xff\r\n",7)+EncodeObservationLine(a)+"\r\n"+
        std::string(700,'x')+"\n"+EncodeObservationLine(b); // uncommitted tail
    Reset();RawPut(raw);
    Check(Commit(c)==ESP_OK,"evidence: malformed repair still commits the new observation");
    Check(ProofBytes(ProofArchive())==raw,"evidence: complete original including NUL CRLF long frame and tail retained");
    Check(Peek(&out)==ESP_OK && out.request_id==a.request_id,"evidence: valid pending predecessor survives repair");
    Check(Remove(a)==ESP_OK && Remove(c)==ESP_OK && ProofBytes(ProofArchive())==raw,
          "evidence: later healthy ACKs never remove the retained original");
    for(bool quarantine:{false,true}) {
        Reset();const std::string damaged="malformed\n"+EncodeObservationLine(a)+'\n'+EncodeObservationLine(b)+'\n';
        RawPut(damaged);
        Check(Remove(a,quarantine)==ESP_OK,"evidence: ACK/quarantine completes after retaining malformed source");
        Check(ProofBytes(ProofArchive())==damaged,"evidence: ACK/quarantine cannot bypass original preservation");
        Check(Peek(&out)==ESP_OK && out.request_id==b.request_id,"evidence: ACK preserves the following valid observation");
    }
    Reset();Commit(a);Commit(b);Remove(a);
    Check(!ProofExists(ProofArchive()) && !ProofExists(ProofArchive()+".tmp"),"evidence: healthy traffic consumes no evidence slots");
    Reset();RawPut(raw);ProofPut(ProofArchive(),raw);ProofPut(ProofArchive()+".tmp","unknown prior stage");
    Check(Commit(c)==ESP_OK && !ProofExists(ProofArchive(1)),"evidence: exact committed copy is reused without another slot");
    Check(ProofBytes(ProofArchive()+".tmp")=="unknown prior stage","evidence: unknown staging file is never promoted or replaced");
    Reset();RawPut(raw);auto different=raw;different[0]='Z';ProofPut(ProofArchive(),different);
    Check(Commit(c)==ESP_OK,"evidence: a different same-size generation uses a fresh slot");
    Check(ProofBytes(ProofArchive())==different && ProofBytes(ProofArchive(1))==raw,
          "evidence: length equality never substitutes a different original");
    Reset();RawPut(raw);ProofPut(ProofArchive(),raw);bad_read_path=ProofArchive();
    Check(Commit(c)!=ESP_OK && ProofBytes(kOutboxPath)==raw,"evidence: retained-copy read error holds repair");
    for(bool linked:{false,true}) {
        Reset();RawPut(raw);
        const int created=linked?symlink(kOutboxPath,ProofArchive().c_str()):mkfifo(ProofArchive().c_str(),0600);
        if(created!=0)std::abort();
        Check(Commit(c)!=ESP_OK && ProofBytes(kOutboxPath)==raw,"evidence: nonregular evidence slot rejects before opening or aliasing it");
    }
    for(bool temporary:{false,true}) {
        Reset();RawPut(raw);
        for(unsigned slot=0;slot<4;++slot) ProofPut(ProofArchive(slot)+(temporary?".tmp":""),"opaque occupied slot "+std::to_string(slot));
        Check(Commit(c)!=ESP_OK && ProofBytes(kOutboxPath)==raw,"evidence: full slots hold repair without changing the source");
        for(unsigned slot=0;slot<4;++slot)
            Check(ProofBytes(ProofArchive(slot)+(temporary?".tmp":""))=="opaque occupied slot "+std::to_string(slot),
                  "evidence: occupied generations remain byte-identical");
    }
    for(unsigned fault=0;fault<8;++fault) {
        Reset();RawPut(raw);const std::string stage=ProofArchive()+".tmp";
        if(fault==0)bad_open_path=stage;
        if(fault==1)partial_write_path=stage;
        if(fault==2)bad_close_path=stage;
        if(fault==3)proof_fail_flush=true;
        if(fault==4)proof_fail_sync=true;
        if(fault==5)proof_fail_rename=true;
        if(fault==6)proof_copy_read_error=true;
        if(fault==7)proof_copy_close_error=true;
        Check(Commit(c)!=ESP_OK,"evidence: source/copy read close create write flush fsync rename errors block repair");
        Check(ProofBytes(kOutboxPath)==raw && !ProofExists(kOutboxBackupPath),"evidence: failure never rotates or overwrites the primary");
    }
    Reset();RawPut(raw);proof_drift_bytes=raw;proof_drift_bytes[0]='X';proof_drift_copy=true;
    Check(Commit(c)!=ESP_OK && ProofBytes(kOutboxPath)==proof_drift_bytes,"evidence: same-size drift since the read proof rejects repair");
    for(bool corrupt_saved:{false,true}) {
        Reset();RawPut(raw);proof_drift_bytes=raw;proof_drift_bytes[0]='Y';
        proof_corrupt_saved=corrupt_saved;proof_drift_publish=!corrupt_saved;
        Check(Commit(c)!=ESP_OK,"evidence: source or saved bytes changed during publication reject repair");
        Check(ProofBytes(kOutboxPath)==(corrupt_saved?raw:proof_drift_bytes),"evidence: final recheck holds the actual current source");
    }
    Reset();RawPut(raw);proof_race_stage=true;
    Check(Commit(c)!=ESP_OK && ProofBytes(kOutboxPath)==raw,"evidence: newly raced stage rejects repair");
    Check(ProofBytes(ProofArchive()+".tmp")==std::string("FOREIGN STAGING BYTES\0",22),"evidence: exclusive creation preserves the raced foreign bytes");
    Reset();RawPut(raw);fail_publish=fail_restore=true;
    Check(Commit(c)!=ESP_OK && ProofBytes(ProofArchive())==raw,"evidence: failed primary publish/restore retains the independent copy");
    fail_publish=fail_restore=false;g_empty_problem_outbox_known=false;
    Check(Commit(c)==ESP_OK && ProofBytes(ProofArchive())==raw && !ProofExists(ProofArchive(1)),"evidence: boot recovery and retry reuse the same original");
    for(int crash:{1,2}) {
        Reset();RawPut(raw);proof_crash=crash;
        const pid_t child=fork();if(child<0)std::abort();
        if(child==0) { Commit(c);_exit(74); }
        int status=0;if(waitpid(child,&status,0)!=child)std::abort();proof_crash=0;
        Check(WIFEXITED(status) && WEXITSTATUS(status)==73,"evidence: actual child exits at the intended persistence boundary");
        Check(ProofBytes(kOutboxPath)==raw,"evidence: child exit before primary mutation retains the source");
        const auto original_stage=ProofBytes(ProofArchive()+".tmp");g_empty_problem_outbox_known=false;
        Check(Commit(c)==ESP_OK,"evidence: a fresh transaction resumes safely after process exit");
        Check(ProofBytes(ProofArchive(crash==1?1:0))==raw,"evidence: interrupted stage remains occupied; committed copy is reused");
        if(crash==1)Check(ProofBytes(ProofArchive()+".tmp")==original_stage && original_stage==raw,
                          "evidence: synced but unpublished stage is neither promoted nor deleted");
    }
    Reset();const std::string too_big(128*1024+1,'x');RawPut(too_big);
    Check(Commit(c)!=ESP_OK && ProofBytes(kOutboxPath)==too_big,"evidence: oversized source is preserved without an unbounded repair");
    Reset();std::string boundary=EncodeObservationLine(a)+'\n';boundary.append(128*1024-boundary.size(),'x');RawPut(boundary);
    Check(Commit(c)==ESP_OK && ProofBytes(ProofArchive())==boundary,"evidence: source at the exact retention limit is supported");
    Reset();const std::string prior="bad\n"+EncodeObservationLine(a)+'\n';RawPut(prior);
    Check(Commit(a)==ESP_OK && ProofBytes(kOutboxPath)==prior && !ProofExists(ProofArchive()),
          "evidence: idempotent no-write retry does not manufacture a repaired source");
    std::printf("Problem evidence: %d checks, %d failures (host only; NOT HIL)\n",
        passes-start_passes+failures-start_failures,failures-start_failures);
    Reset();
}
