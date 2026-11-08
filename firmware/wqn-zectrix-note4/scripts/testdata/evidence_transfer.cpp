// Actual export worker + transfer + raw reader. Only SDK/hardware edges are ports.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/select.h>
#include "services/storage_recovery_export.h"
#include "runtime/raw_flash_evidence.h"
#include "esp_flash.h"
#include "esp_app_desc.h"
std::string mode;
unsigned reads=0, opens=0, allocations=0, progress_bytes=0, yields=0;
size_t flash_size=32769;
std::string logical_wire;
bool owner=true;
// Owner-thread-only clock fault: expire after a complete bad control line
// when parsing has not resumed output, instead of racing host fsync latency.
int64_t control_clock_offset=0;
bool control_has_nul=false, bad_control_complete=false, output_after_bad_control=false;
void Destination(const void*) {}
int64_t esp_timer_get_time() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() * (mode=="final-retry" ? 50 : 1) + control_clock_offset;
}
void vTaskDelay(unsigned) { ++yields; std::this_thread::yield(); }
void* heap_caps_malloc(size_t size, unsigned flags) {
    if (flags != 5) std::abort();
    if (mode=="oom") return nullptr;
    ++allocations; return std::malloc(size);
}
void heap_caps_free(void* data) { --allocations; std::free(data); }
void esp_fill_random(void* data, size_t size) { std::memset(data,0x11,size); }
esp_err_t esp_read_mac(uint8_t* data, int) { std::memset(data,0x22,6); return ESP_OK; }
const esp_app_desc_t* esp_app_get_description() { static esp_app_desc_t app; std::memset(app.app_elf_sha256,0x33,32); return &app; }
namespace wqn::services { bool IsStorageServiceTask() { return owner; } }
namespace wqn::runtime {
bool HasActiveStorageTempFileLease() { return false; }
bool HasActivePackStreamPath() { return false; }
// Logical collector has separate full production coverage. This fixture tests
// its worker dispatch refusal; transport success below exercises real raw code.
esp_err_t CaptureStorageEvidenceOnOwner(const StorageEvidenceIdentity&, const StorageEvidenceSink& sink, StorageEvidenceSummary* summary) {
    if (mode!="logical-success") return ESP_ERR_NOT_SUPPORTED;
    std::ifstream input(logical_wire,std::ios::binary);
    uint8_t frame[kStorageEvidenceMaxFrameBytes];
    while (input.read(reinterpret_cast<char*>(frame),11)) {
        const unsigned length=frame[5] | (unsigned(frame[6])<<8);
        if (length+11>sizeof(frame) || !input.read(reinterpret_cast<char*>(frame+11),length)) return ESP_FAIL;
        const int error=sink.emit(frame,length+11,sink.context);
        if (error!=ESP_OK) return error;
    }
    summary->capture_complete=true; return ESP_OK;
}
}
namespace wqn::recovery_usb {
esp_err_t Reserve() { ++opens; return mode=="open-error" ? ESP_FAIL : ESP_OK; }
int Write(const uint8_t* data, size_t size, uint32_t ms) {
    if (ms>50) std::abort();
    if (bad_control_complete && size>=8 &&
        (std::memcmp(data,"WQNT1 D ",8)==0 || std::memcmp(data,"WQNT1 F ",8)==0))
        output_after_bad_control=true;
    if (mode=="zero-write") return 0;
    if (mode=="write-error") return -1;
    if (mode=="late-fail" && size>=8 && std::memcmp(data,"WQNT1 F",7)==0) return -1;
    return ::write(1,data,std::min<size_t>(size,mode=="partial" ? 7 : 128));
}
int Read(uint8_t* data, size_t size, uint32_t ms) {
    if (mode.rfind("nul-",0)==0 && bad_control_complete && !output_after_bad_control) {
        control_clock_offset=30000000;
        return 0;
    }
    fd_set set; FD_ZERO(&set); FD_SET(0,&set);
    timeval time{0,static_cast<long>(ms*1000)};
    const int selected=select(1,&set,nullptr,nullptr,&time);
    if (selected<0) return -1;
    if (selected==0) return 0;
    const int count=::read(0,data,std::min<size_t>(size,13));
    for (int i=0;i<count;++i) {
        if (data[i]==0) control_has_nul=true;
        if (data[i]=='\n') {
            if (control_has_nul) { bad_control_complete=true; output_after_bad_control=false; }
            control_has_nul=false;
        }
    }
    return count==0 ? -1 : count;
}
}
esp_err_t esp_flash_get_size(esp_flash_t*,uint32_t* size) { *size=flash_size; return ESP_OK; }
esp_err_t esp_flash_get_physical_size(esp_flash_t*,uint32_t* size) { *size=flash_size; return ESP_OK; }
bool esp_flash_encryption_enabled() { return false; }
esp_err_t esp_flash_read(esp_flash_t*,void* data,uint32_t address,uint32_t size) {
    ++reads;
    if (mode=="source-error" && address==384) return ESP_FAIL;
    for (uint32_t i=0;i<size;++i) static_cast<uint8_t*>(data)[i]=static_cast<uint8_t>((address+i)*17+29);
    return ESP_OK;
}
bool Cancel(void*) { return mode=="cancel-early"; }
void Progress(uint32_t bytes,void*) { if (bytes<progress_bytes) std::abort(); progress_bytes=bytes; }
int main(int argc,char** argv) {
    if (argc<2 || argc>3) return 2;
    if (argc==3) logical_wire=argv[2];
    mode=argv[1]; if (mode=="large") flash_size=1024*1024;
    owner=mode!="off-owner";
    if (mode.rfind("manual-",0)==0) {
        wqn::runtime::EnterStorageEvidenceBootPreservation(mode=="manual-error" ? ESP_FAIL : ESP_OK);
        if (wqn::runtime::EnterStorageEvidenceBootRecovery(ESP_FAIL,99)!=ESP_ERR_INVALID_STATE) std::abort();
    } else if (mode!="not-mode") wqn::runtime::EnterStorageEvidenceBootRecovery(ESP_ERR_INVALID_CRC,3);
    wqn::services::StorageRecoveryExportResult result;
    const auto kind=mode=="invalid-kind" ? static_cast<wqn::services::StorageRecoveryExportKind>(99) :
        (mode=="logical" || mode=="logical-success" || mode=="manual-logical") ? wqn::services::StorageRecoveryExportKind::kLogical : wqn::services::StorageRecoveryExportKind::kRaw;
    const int64_t budget=mode=="final-retry" ? 1500000000LL : (mode=="no-ready" || mode=="zero-write") ? 200000 : mode=="timeout-ack" ? 350000 : 15000000;
    const int error=wqn::services::RunStorageRecoveryExportOnOwner(kind,Cancel,nullptr,
        mode=="expired" ? 1 : esp_timer_get_time()+budget,Progress,&result);
    std::fprintf(stderr,"{\"error\":%d,\"source_error\":%d,\"transfer_error\":%d,\"ack\":%s,\"complete\":%s,\"reads\":%u,\"opens\":%u,\"leaks\":%u,\"bytes\":%u,\"progress\":%u,\"yields\":%u}\n",
        error,result.source_error,result.transfer_error,result.commit_acknowledged ? "true":"false",result.source_complete ? "true":"false",reads,opens,allocations,result.persisted_data_bytes,progress_bytes,yields);
    return 0;
}
