#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
@@HEADER@@
extern "C" uint32_t esp_rom_crc32_le(uint32_t,const uint8_t*,uint32_t);
void Log(const char*,const char*,...) {}
#define ESP_LOGW(...) Log(__VA_ARGS__)
constexpr char kTag[]="forensics-codec";
@@CONSTANTS@@
@@RECORDS@@
@@FUNCTIONS@@
@@DESCRIPTOR_PATHS@@
constexpr size_t kNativeCapacity = @@CAPACITY@@;

void Put(const std::string& path,const std::vector<OutboxRecord>& records,size_t tail=0) {
    FILE* file=std::fopen(path.c_str(),"wb"); if(!file)std::abort();
    for(const auto& record:records) {
        if(std::fwrite(&record,1,sizeof(record),file)!=sizeof(record))std::abort();
    }
    if(tail && std::fwrite(&records.front(),1,tail,file)!=tail)std::abort();
    if(std::fclose(file)!=0)std::abort();
}
OutboxRecord Observation(const char* request,uint64_t sequence) {
    @@OBSERVATION_TYPE@@ value;
    value.request_id=request;value.sequence=sequence;
    value.session_id="11111111-1111-1111-1111-111111111111";
    value.item_id="22222222-2222-2222-2222-222222222222";
    value.occurred_at="2026-11-05T00:00:00Z";value.next_position=17;
    @@EXTRA@@
    OutboxRecord record{};
    if(BuildObservationRecord(value,OutboxRecordKind::kObservation,&record)!=0)std::abort();
    return record;
}
int main(int argc,char** argv) {
    if(argc!=2)return 2;
    const auto one=Observation("request-one-00001",7),two=Observation("request-two-00002",8),
               three=Observation("request-three-003",9);
    auto ack=one;ack.kind=static_cast<uint8_t>(OutboxRecordKind::kAck);ack.crc=RecordCrc(ack);
    OutboxRecord suspended{};
    if(BuildSuspendRecord(two.request_id,wqn::OutboxSuspendReason::kActorOwnership,&suspended)!=0)return 2;
    const std::vector<OutboxRecord> healthy{one,ack,two,suspended,three};
    std::printf("{\"record_bytes\":%zu,\"request_offset\":%zu,\"capacity\":%zu,\"crc_vector\":%lu}\n",
        sizeof(OutboxRecord),offsetof(OutboxRecord,request_id),kNativeCapacity,
        static_cast<unsigned long>(Crc32("123456789",9)));
    @@DESCRIPTOR_EMIT@@
    const auto emit=[&](const char* name,const std::vector<OutboxRecord>& records,size_t tail=0) {
        const std::string path=std::string(argv[1])+"/"+name;
        Put(path,records,tail);
        OutboxScan scan;const auto error=ScanOutboxFile(path.c_str(),&scan);
        std::printf("{\"file\":\"%s\",\"error\":%d,\"pending\":%zu,\"acknowledged\":%zu,"
            "\"suspended\":%zu,\"total_records\":%zu,\"ack_records\":%zu,\"suspend_records\":%zu,"
            "\"orphan_ack_records\":%zu,\"orphan_suspend_records\":%zu,\"partial_tail\":%s}\n",
            name,error,scan.pending.size(),scan.acknowledged.size(),scan.suspended.size(),scan.total_records,
            scan.ack_records,scan.suspend_records,scan.orphan_ack_records,scan.orphan_suspend_records,
            scan.partial_tail?"true":"false");
    };
    emit("healthy.bin",healthy);emit("empty.bin",{});emit("torn.bin",healthy,17);
    for(const auto& which:std::vector<std::pair<size_t,const char*>>{{0,"bad-observation.bin"},
            {1,"bad-ack.bin"},{3,"bad-suspend.bin"}}) {
        auto broken=healthy;broken[which.first].crc^=1;emit(which.second,broken);
    }
    auto changed=one;changed.version=2;changed.crc=RecordCrc(changed);emit("future.bin",{changed});
    changed=one;changed.kind=9;changed.crc=RecordCrc(changed);emit("unknown-kind.bin",{changed});
    changed=one;changed.mode=99;changed.crc=RecordCrc(changed);emit("invalid-mode.bin",{changed});
    changed=one;changed.mode=static_cast<uint8_t>(@@MODE_MAX@@);
    changed.action=static_cast<uint8_t>(@@ACTION_MAX@@);
    changed.crc=RecordCrc(changed);emit("max-enums.bin",{changed});
    changed=one;changed.request_id[64]='x';changed.crc=RecordCrc(changed);emit("no-terminator.bin",{changed});
    changed=one;changed.request_id[20]='x';changed.crc=RecordCrc(changed);emit("opaque-padding.bin",{changed});
    changed=one;changed.request_id[0]=static_cast<char>(0xff);changed.crc=RecordCrc(changed);emit("opaque-utf8.bin",{changed});
    changed=one;changed.next_position=55;changed.crc=RecordCrc(changed);emit("conflict.bin",{one,changed});
    emit("duplicate.bin",{one,one});emit("orphan-markers.bin",{ack,suspended});
    std::vector<OutboxRecord> capacity;
    for(unsigned n=0;n<=kNativeCapacity;++n) {
        char id[40];std::snprintf(id,sizeof(id),"capacity-request-%04u",n);
        capacity.push_back(Observation(id,n));
    }
    emit("at-capacity.bin",{capacity.begin(),capacity.end()-1});
    emit("over-capacity.bin",capacity);
    if(kNativeCapacity>1000) {
        emit("legacy-limit-exceeded.bin",{capacity.begin(),capacity.begin()+1001});
        std::vector<OutboxRecord> parked;
        for(size_t n=0;n<kNativeCapacity;++n) {
            parked.push_back(capacity[n]);
            OutboxRecord marker{};
            if(BuildSuspendRecord(capacity[n].request_id,wqn::OutboxSuspendReason::kActorOwnership,&marker)!=0)std::abort();
            parked.push_back(marker);
        }
        emit("capacity-with-suspend-markers.bin",parked);
    }
}
