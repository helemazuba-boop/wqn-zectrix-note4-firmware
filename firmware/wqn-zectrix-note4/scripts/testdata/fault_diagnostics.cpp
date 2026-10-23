#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>
#include "esp_system.h"
#include "freertos/FreeRTOS.h"

static std::atomic<unsigned> checks{0}, allocations{0};
static std::atomic<bool> watch_allocations{false};
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); std::abort(); } } while (0)
void* operator new(size_t size) {
    if (watch_allocations.load()) ++allocations;
    if (void* p=std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p,size_t) noexcept { std::free(p); }
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete[](void* p,size_t) noexcept { ::operator delete(p); }
static std::atomic<int64_t> clock_us{1000000};
static std::time_t wall_seconds=1792713600;
static esp_reset_reason_t reset_reason=ESP_RST_POWERON;
static thread_local unsigned lock_depth=0;
static std::vector<std::string> logs;
int64_t esp_timer_get_time() { return clock_us.load(); }
extern "C" std::time_t __wrap_time(std::time_t* out) { if(out) *out=wall_seconds; return wall_seconds; }
esp_reset_reason_t esp_reset_reason() { return reset_reason; }
void FaultHostEnter(portMUX_TYPE* lock) { CHECK(lock_depth==0); lock->lock(); ++lock_depth; }
void FaultHostExit(portMUX_TYPE* lock) { CHECK(lock_depth==1); --lock_depth; lock->unlock(); }
void FaultHostLog(const char*,const char* format,...) {
    CHECK(lock_depth==0);
    char line[1024]; va_list args; va_start(args,format); std::vsnprintf(line,sizeof(line),format,args); va_end(args);
    logs.emplace_back(line);
}
uint32_t esp_rom_crc32_le(uint32_t initial,const uint8_t* bytes,size_t size) {
    uint32_t crc=~initial;
    for(size_t i=0;i<size;++i) { crc^=bytes[i]; for(int bit=0;bit<8;++bit) crc=(crc>>1)^(0xedb88320U & (0U-(crc&1))); }
    return ~crc;
}
void* FaultHostCopy(void*,const void*,size_t);
#include "runtime/wall_clock.cpp"
#include "fault_diagnostics.cpp"
#include "error_recorder.cpp"
namespace fd=wqn::runtime;
struct InjectedReset {};
static int cut_after=-1;
void* FaultHostCopy(void* destination,const void* source,size_t size) {
    const auto address=reinterpret_cast<uintptr_t>(destination);
    const auto first=reinterpret_cast<uintptr_t>(fd::g_copies);
    if(cut_after>=0 && address>=first && address<first+sizeof(fd::g_copies)) {
        CHECK(static_cast<size_t>(cut_after)<=size);
        std::memcpy(destination,source,cut_after); cut_after=-1; throw InjectedReset{};
    }
    return std::memcpy(destination,source,size);
}
static void Boot(esp_reset_reason_t reason,bool fresh=false) {
    CHECK(lock_depth==0);
    if(fresh) std::memset(fd::g_copies,0,sizeof(fd::g_copies));
    std::memset(&fd::g_current,0,sizeof(fd::g_current));
    fd::g_initialized=false; fd::g_active_copy=1;
    for(auto& record:wqn::g_error_ring) record={};
    wqn::g_error_seq=0;
    fd::g_wall_clock_state=0; fd::g_rtc_anchor_seconds=0; fd::g_rtc_anchor_monotonic_us=0;
    clock_us=1000000; reset_reason=reason; cut_after=-1;
    fd::InitFaultDiagnostics();
}
static auto Snapshot() { fd::FaultDiagnosticsSnapshot view{}; CHECK(fd::CopyFaultDiagnostics(&view)); return view; }
static void Record(int code,uint32_t op=42,uint32_t generation=7) {
    fd::RecordFault(fd::FaultDomain::kStorage,fd::FaultPhase::kStorageExecution,code,op,generation);
}
static void Basic() {
    CHECK(esp_rom_crc32_le(0,reinterpret_cast<const uint8_t*>("123456789"),9)==0xcbf43926);
    Boot(ESP_RST_POWERON,true); auto view=Snapshot(); CHECK(view.count==0 && !view.recovered && view.boot_id==1);
    CHECK(!fd::CopyFaultDiagnostics(nullptr));
    fd::SetFaultBootPhase(fd::BootPhase::kStorage); fd::NoteBuildTimeApplied();
    watch_allocations=true; const auto before=allocations.load(); Record(-19,123,9); watch_allocations=false;
    CHECK(allocations==before && logs.empty());
    view=Snapshot(); CHECK(view.count==1 && view.reports==1 && view.overwritten==0);
    const auto original=view.records[0];
    CHECK(original.code==-19 && original.operation==123 && original.generation==9);
    CHECK(original.uptime_ms==1000 && original.unix_sec==wall_seconds && original.boot_id==1);
    CHECK(original.clock_source==static_cast<uint8_t>(fd::WallClockSource::kBuildTime) && original.clock_epoch==1);
    CHECK(original.boot_phase==static_cast<uint8_t>(fd::BootPhase::kStorage));
    CHECK(!std::strcmp(original.build,"host1234"));
    Boot(ESP_RST_SW); view=Snapshot();
    CHECK(view.recovered && view.boot_id==2 && view.previous_boot_id==1 && view.previous_boot_phase==fd::BootPhase::kStorage);
    CHECK(view.count==1 && view.records[0].boot_id==1 && view.records[0].code==original.code);
    CHECK(!std::strcmp(view.previous_build,"host1234"));
    fd::NoteRtcTimeRestored(true,11,wall_seconds); Record(-20,124,10); view=Snapshot();
    CHECK(view.records[1].clock_source==static_cast<uint8_t>(fd::WallClockSource::kRtcSntp));
    CHECK(view.records[1].clock_epoch==11 && view.records[1].rtc_continuity==1 && view.records[1].boot_id==2);
    for(auto reason:{ESP_RST_SW,ESP_RST_PANIC,ESP_RST_INT_WDT,ESP_RST_TASK_WDT,ESP_RST_WDT,ESP_RST_DEEPSLEEP,ESP_RST_CPU_LOCKUP}) {
        Boot(reason); view=Snapshot(); CHECK(view.recovered && view.count==2 && view.records[0].boot_id==1);
    }
    for(auto reason:{ESP_RST_UNKNOWN,ESP_RST_POWERON,ESP_RST_EXT,ESP_RST_BROWNOUT,ESP_RST_SDIO,ESP_RST_USB,ESP_RST_JTAG,ESP_RST_EFUSE,ESP_RST_PWR_GLITCH}) {
        Record(-21); Boot(reason); view=Snapshot(); CHECK(!view.recovered && view.count==0 && view.reports==0 && view.boot_id==1);
    }
}
static void ErrorsAndPrivacy() {
    Boot(ESP_RST_POWERON,true); logs.clear();
    constexpr char secret[]="do-not-retain-token-abcdefgh";
    const auto before=allocations.load(); watch_allocations=true;
    wqn::RecordError("audio_cap","failure %s",secret); watch_allocations=false;
    CHECK(before==allocations && logs.empty());
    auto view=Snapshot(); CHECK(view.count==1 && view.records[0].domain==fd::FaultDomain::kAudio);
    CHECK(view.records[0].code==fd::kFaultCodeUnavailable && view.records[0].phase==fd::FaultPhase::kReportedError);
    wqn::ErrorRecord text[8]{}; CHECK(wqn::CopyRecentErrors(text,8)==1 && std::strstr(text[0].msg,secret));
    wqn::RecordError(secret,"unknown domain");
    CHECK(Snapshot().records[1].domain==fd::FaultDomain::kOther);
    const auto* raw=reinterpret_cast<const char*>(fd::g_copies);
    CHECK(std::search(raw,raw+sizeof(fd::g_copies),secret,secret+std::strlen(secret))==raw+sizeof(fd::g_copies));
    Boot(ESP_RST_SW); CHECK(wqn::CopyRecentErrors(text,8)==0 && Snapshot().count==2);
    fd::DumpFaultDiagnosticsToLog(); CHECK(logs.size()==3 && logs[0].find("FAULT:boot")!=std::string::npos);
    for(const auto& line:logs) CHECK(line.find(secret)==std::string::npos);
    const auto count=Snapshot().count; wqn::RecordError(nullptr,"x"); wqn::RecordError("x",nullptr);
    fd::RecordLegacyFault(nullptr); CHECK(Snapshot().count==count);
    fd::RecordFault(static_cast<fd::FaultDomain>(255),fd::FaultPhase::kReportedError,1);
    CHECK(Snapshot().count==count);
}
static void RingAndCounters() {
    Boot(ESP_RST_POWERON,true);
    for(int i=1;i<=100;++i) Record(i,i);
    auto view=Snapshot(); CHECK(view.count==4 && view.reports==100 && view.overwritten==96);
    for(unsigned i=0;i<4;++i) CHECK(view.records[i].sequence==97+i && view.records[i].code==97+static_cast<int>(i));
    // Exercise revision/record/boot wrap without requiring billions of reports.
    fd::g_current.revision=UINT32_MAX-1; fd::g_current.fault_sequence=UINT32_MAX-1;
    fd::g_current.view.boot_id=UINT32_MAX; fd::g_current.view.reports=UINT32_MAX;
    fd::g_current.view.overwritten=UINT32_MAX; fd::CommitLocked();
    Record(101); Record(102); Boot(ESP_RST_SW); view=Snapshot();
    CHECK(view.boot_id==1 && view.previous_boot_id==UINT32_MAX && view.count==4);
    CHECK(view.reports==UINT32_MAX && view.overwritten==UINT32_MAX);
    CHECK(view.records[2].sequence==UINT32_MAX && view.records[3].sequence==1);
}
static void Corruption() {
    Boot(ESP_RST_POWERON,true); Record(11); Record(22);
    const auto latest=fd::g_active_copy; fd::g_copies[latest].view.records[1].code=999;
    Boot(ESP_RST_SW); auto view=Snapshot();
    CHECK(view.recovered && view.count==1 && view.records[0].code==11 && view.integrity_rejections==1);
    // A checksum-valid but structurally invalid slot is also refused.
    fd::g_copies[fd::g_active_copy].view.count=500;
    fd::g_copies[fd::g_active_copy].crc=fd::Crc(fd::g_copies[fd::g_active_copy]);
    Boot(ESP_RST_SW); CHECK(Snapshot().count<=4);
    std::memset(fd::g_copies,0xa5,sizeof(fd::g_copies)); Boot(ESP_RST_PANIC);
    view=Snapshot(); CHECK(!view.recovered && view.count==0 && view.integrity_rejections==2);
    CHECK(fd::Valid(fd::g_copies[fd::g_active_copy]));
}
static void TornWrites() {
    Boot(ESP_RST_POWERON,true); Record(77);
    fd::Journal originals[2]; std::memcpy(originals,fd::g_copies,sizeof(originals));
    const auto old_active=fd::g_active_copy;
    for(size_t cut=0;cut<=sizeof(fd::Journal)-sizeof(uint32_t);++cut) {
        std::memcpy(fd::g_copies,originals,sizeof(originals));
        std::memcpy(&fd::g_current,&originals[old_active],sizeof(fd::g_current));
        fd::g_active_copy=old_active; fd::g_initialized=true;
        cut_after=static_cast<int>(cut); bool interrupted=false;
        try { Record(88); } catch(const InjectedReset&) {
            CHECK(lock_depth==1); lock_depth=0; fd::g_lock.unlock(); interrupted=true;
        }
        CHECK(interrupted); Boot(ESP_RST_SW); const auto view=Snapshot();
        CHECK(view.recovered && view.count==1 && view.records[0].code==77);
    }
}
static void ConcurrentReports() {
    Boot(ESP_RST_POWERON,true);
    std::vector<std::thread> threads;
    for(unsigned writer=0;writer<4;++writer) threads.emplace_back([writer] {
        for(unsigned i=0;i<100;++i) Record(static_cast<int>(writer),writer+100,i+1);
    });
    std::thread reader([] {
        for(unsigned i=0;i<100;++i) { const auto view=Snapshot(); CHECK(view.count<=4 && view.boot_id==1); }
    });
    for(auto& thread:threads) thread.join();
    reader.join();
    const auto view=Snapshot(); CHECK(view.reports==400 && view.overwritten==396 && view.count==4);
    for(unsigned i=0;i<4;++i) {
        CHECK(view.records[i].sequence==397+i);
        CHECK(view.records[i].operation==100+static_cast<unsigned>(view.records[i].code));
    }
    Boot(ESP_RST_PANIC); CHECK(Snapshot().reports==400 && Snapshot().count==4);
}
int main() {
    Basic(); ErrorsAndPrivacy(); RingAndCounters(); Corruption(); TornWrites(); ConcurrentReports();
    std::printf("Fault RTC journal/error/clock: %u checks PASS, RTC=%zu B cache=%zu B; host ports, not retention HIL\n",checks.load(),sizeof(fd::g_copies),sizeof(fd::g_current));
}
