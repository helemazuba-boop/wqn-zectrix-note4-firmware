#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "services/audio_service.h"
#include "services/audio_codec_deadline.h"
#include "i2c_bus_lock.h"
#include "driver/i2c_master.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); std::exit(1); } } while (0)
static int64_t clock_us=1000000, expected_deadline;
static int64_t io_cost=100, lock_cost, reset_cost, remove_cost;
static unsigned active_ops, probes, adds, removes, resets, transfers, takes, gives, denials;
static unsigned fail_at, late_at, failures_each_register, close_admission_at;
static bool fail_all, fail_attach, late_probe, remove_fails, registry_fails, standalone;
static bool deny_locks;
static int bus_token, device_token;
static std::array<uint8_t,256> regs{};
static uint32_t io_hash=2166136261U;
static void HashIo(uint8_t byte) { io_hash=(io_hash^byte)*16777619U; }
static std::vector<uint8_t> write_order;
struct FixtureMutex { unsigned depth=0; };
static FixtureMutex mutex;
@@RESOURCE_SLOT@@
static ResourceSlot g_codec_slots[2];
static int g_resource_lock;
static std::atomic<uint32_t> g_inflight_operations{0};
#define taskENTER_CRITICAL(p) ((void)(p))
#define taskEXIT_CRITICAL(p) ((void)(p))
static std::atomic<bool> g_codec_powered{true}, g_codec_configuration_valid{false};
static std::atomic<int> g_codec_configuration_volume{-1};
static std::atomic<wqn::services::AudioCodecProfile> g_codec_configuration_profile{wqn::services::AudioCodecProfile::kCapture};
static std::atomic<bool> g_accept_operations{true};
static std::atomic<wqn::services::AudioState> g_state{wqn::services::AudioState::kCapturing};
static uint32_t valid_session=1;
constexpr char kTag[]="codec-host";
#define ESP_RETURN_ON_ERROR(expr,...) do { auto result_=(expr); if(result_!=ESP_OK)return result_; } while(0)
constexpr int kI2cSda=47, kI2cScl=48;
@@CONSTANTS@@
using wqn::services::detail::CodecDeadline;

int64_t esp_timer_get_time() { return clock_us; }
void FixtureLog(const char*, const char*, ...) {}
int gpio_get_level(int) { return 1; }
namespace wqn {
void RecordError(const char*, const char*, ...) {}
void* GetSharedI2cBusHandle() { return &bus_token; }
}
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() { return &mutex; }
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t value, TickType_t ticks)
{
    CHECK(value==&mutex);
#ifndef FIXTURE_HAPPY_ONLY
    CHECK(ticks<=pdMS_TO_TICKS(100));
#endif
    ++takes; clock_us+=lock_cost;
    if(deny_locks && mutex.depth==0) {
        ++denials; clock_us+=int64_t{ticks}*1000000/configTICK_RATE_HZ; return pdFALSE;
    }
    ++mutex.depth;
    return pdTRUE;
}
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t value)
{
    CHECK(value==&mutex && mutex.depth>0); --mutex.depth; ++gives; return pdTRUE;
}
void vTaskDelay(TickType_t ticks)
{
    CHECK(ticks>0);
    if(standalone) CHECK(mutex.depth==0);
    const int64_t tick_us=1000000/configTICK_RATE_HZ;
    clock_us+=int64_t{ticks}*tick_us-clock_us%tick_us;
}
static void TransferBudget(int milliseconds)
{
    CHECK(active_ops>0 && mutex.depth>0 && milliseconds>0 && milliseconds<=100);
#ifndef FIXTURE_HAPPY_ONLY
    CHECK(clock_us<expected_deadline);
    CHECK(int64_t{milliseconds}*1000<=expected_deadline-clock_us);
#endif
}
esp_err_t i2c_master_probe(void* bus,uint16_t address,int ms)
{
    CHECK(bus==&bus_token && address==0x18); TransferBudget(ms); ++probes;
    clock_us+=io_cost;
    if(late_probe) clock_us=expected_deadline;
    return ESP_OK;
}
esp_err_t i2c_master_bus_add_device(void* bus,const i2c_device_config_t* cfg,void** dev)
{
    CHECK(bus==&bus_token && cfg->device_address==0x18 && cfg->scl_speed_hz==100000);
    ++adds; if(fail_attach)return ESP_FAIL; *dev=&device_token; return ESP_OK;
}
esp_err_t i2c_master_bus_rm_device(void* device)
{
    CHECK(device==&device_token); ++removes; clock_us+=remove_cost;
    return remove_fails?ESP_FAIL:ESP_OK;
}
esp_err_t i2c_master_bus_reset(void* bus)
{
    CHECK(bus==&bus_token && mutex.depth>0 && clock_us<expected_deadline);
    ++resets; clock_us+=reset_cost;
    return ESP_OK;
}
static bool DoTransfer(void* dev,int ms)
{
    CHECK(dev==&device_token); TransferBudget(ms); ++transfers; clock_us+=io_cost;
    if(transfers==close_admission_at)g_accept_operations=false;
    if(transfers==late_at)clock_us=expected_deadline;
    return !fail_all && transfers!=fail_at &&
        !(failures_each_register && transfers%10<=failures_each_register && transfers%10!=0);
}
esp_err_t i2c_master_transmit(void* dev,const uint8_t* bytes,size_t size,int ms)
{
    CHECK(size==2); if(!DoTransfer(dev,ms))return ESP_ERR_TIMEOUT;
    HashIo(0x57);HashIo(bytes[0]);HashIo(bytes[1]);
    regs[bytes[0]]=bytes[1]; write_order.push_back(bytes[0]); return ESP_OK;
}
esp_err_t i2c_master_transmit_receive(void* dev,const uint8_t* reg,size_t wn,uint8_t* out,size_t rn,int ms)
{
    CHECK(wn==1 && rn==1); if(!DoTransfer(dev,ms))return ESP_ERR_TIMEOUT;
    HashIo(0x52);HashIo(*reg);HashIo(regs[*reg]);
    *out=regs[*reg]; return ESP_OK;
}
namespace {
class DriverOperation {
public:
    enum class Kind { kSession, kCodec };
    DriverOperation(const wqn::services::AudioSession& s,Kind kind=Kind::kSession,void* dev=nullptr)
      : ok_(s.id==valid_session && s.id!=0 && g_accept_operations.load() &&
           (kind!=Kind::kCodec || (dev==&device_token && g_codec_slots[0].handle==dev &&
             g_codec_slots[0].owner_session_id==s.id && !g_codec_slots[0].closing)))
    { if(ok_) { ++active_ops; if(kind==Kind::kCodec)slot_=&g_codec_slots[0]; if(slot_)++slot_->inflight; } }
    ~DriverOperation() { if(ok_) { --active_ops; if(slot_)--slot_->inflight; } }
    explicit operator bool()const { return ok_; }
private: bool ok_; ResourceSlot* slot_=nullptr;
};
void* AsBus(void* b) { return b; }
void* AsCodec(void* d) { return d; }
esp_err_t ApplyCodecPower(bool on) { CHECK(on); return ESP_OK; }
@@RETRYABLE@@
esp_err_t RegisterResource(ResourceSlot*,size_t,const wqn::services::AudioSession&,void* dev)
{
    if(registry_fails)return ESP_FAIL;
    CHECK(g_codec_slots[0].handle==nullptr);g_codec_slots[0].handle=dev;
    g_codec_slots[0].owner_session_id=valid_session;return ESP_OK;
}
bool SessionMatches(const wqn::services::AudioSession& session) { return session.id==valid_session; }
@@CLOSE_HELPERS@@
@@INTERNALS@@
}
namespace wqn::services {
const char* AudioStateName(AudioState) { return "active"; }
esp_err_t GetSharedAudioBus(const AudioSession&,AudioBusHandle* out) { *out=&bus_token;return ESP_OK; }
@@PUBLIC@@
}
static void Clear()
{
    CHECK(active_ops==0 && mutex.depth==0 && g_inflight_operations==0);
    g_codec_slots[0]={};g_codec_configuration_valid=false;g_codec_configuration_volume=-1;
    g_codec_powered=true;g_accept_operations=true;valid_session=1;
    clock_us=1000000;expected_deadline=clock_us+kCallTimeoutUs;
    io_cost=100;lock_cost=reset_cost=remove_cost=0;
    probes=adds=removes=resets=transfers=takes=gives=denials=0;
    fail_at=late_at=failures_each_register=close_admission_at=0;
    fail_all=fail_attach=late_probe=remove_fails=registry_fails=standalone=deny_locks=false;
    regs.fill(0);write_order.clear();io_hash=2166136261U;
}
static esp_err_t Configure(wqn::services::AudioCodecProfile profile=wqn::services::AudioCodecProfile::kCapture,int volume=100)
{
    expected_deadline=clock_us+kCallTimeoutUs;
    auto result=wqn::services::ConfigureAudioCodec({1,wqn::services::AudioActivity::kCapture},profile,volume);
    CHECK(active_ops==0 && mutex.depth==0 && takes==gives+denials && g_inflight_operations==0);
    return result;
}
static void Happy()
{
    using namespace wqn::services;
    for(auto profile:{AudioCodecProfile::kCapture,AudioCodecProfile::kPlayback,AudioCodecProfile::kDuplex}) {
        Clear();CHECK(Configure(profile)==ESP_OK);
        CHECK(g_codec_configuration_valid && g_codec_slots[0].handle==nullptr && removes==1);
        CHECK(regs[0x16]==0x24 && write_order[0]==0x44 && write_order[1]==0x44);
        CHECK(regs[0x15]==(profile==AudioCodecProfile::kPlayback?0:0x40));
        CHECK(regs[0x37]==(profile==AudioCodecProfile::kPlayback?0x16:0x08));
        const auto cold_us=clock_us-1000000; const auto io=transfers;
        CHECK(Configure(profile)==ESP_OK && transfers==io && clock_us-1000000==cold_us);
        std::printf("Codec %d Hz profile=%u cold_model_us=%lld transfers=%u io_hash=%08x warm_added_us=0\n",
            configTICK_RATE_HZ,unsigned(profile),static_cast<long long>(cold_us),transfers,io_hash);
    }
}
#ifndef FIXTURE_HAPPY_ONLY
static void Faults()
{
    using namespace wqn::services;
    Clear();CHECK(Configure()==ESP_OK);
    CHECK(ConfigureAudioCodec({2,AudioActivity::kCapture},AudioCodecProfile::kCapture,100)==ESP_ERR_INVALID_STATE);
    g_accept_operations=false;
    CHECK(ConfigureAudioCodec({1,AudioActivity::kCapture},AudioCodecProfile::kCapture,100)==ESP_ERR_INVALID_STATE);
    Clear();fail_all=true;io_cost=300000;
    CHECK(Configure()==ESP_ERR_TIMEOUT && clock_us>=expected_deadline);
    CHECK(!g_codec_configuration_valid && g_codec_slots[0].handle==nullptr && removes>=1);
    Clear();failures_each_register=9;io_cost=100000;
    CHECK(Configure()==ESP_ERR_TIMEOUT && clock_us>=expected_deadline);
    CHECK(!g_codec_configuration_valid && g_codec_slots[0].handle==nullptr);
    Clear();late_probe=true;
    CHECK(Configure()==ESP_ERR_TIMEOUT && adds==0 && transfers==0);
    for(unsigned late:{1U,10U,30U}) {
        Clear();late_at=late;
        CHECK(Configure()==ESP_ERR_TIMEOUT && transfers==late && removes==1);
        CHECK(g_codec_slots[0].handle==nullptr && !g_codec_configuration_valid);
    }
    Clear();remove_cost=kCallTimeoutUs;
    CHECK(Configure()==ESP_ERR_TIMEOUT && !g_codec_configuration_valid && g_codec_slots[0].handle==nullptr);
    Clear();lock_cost=kCallTimeoutUs;
    CHECK(Configure()==ESP_ERR_TIMEOUT && probes==0 && adds==0);
    Clear();deny_locks=true;
    CHECK(Configure()==ESP_ERR_TIMEOUT && probes==0 && adds==0 && denials==1);
    CHECK(clock_us-1000000==100000);
    Clear();fail_at=1;reset_cost=kCallTimeoutUs;
    CHECK(Configure()==ESP_ERR_TIMEOUT && transfers==1 && resets==1 && removes==1);
    Clear();fail_at=1;
    CHECK(Configure()==ESP_OK && removes==1 && resets==1);
    Clear();close_admission_at=1;
    CHECK(Configure()==ESP_ERR_INVALID_STATE && transfers==1 && removes==1);
    CHECK(g_codec_slots[0].handle==nullptr && !g_codec_configuration_valid && resets==0);
    Clear();g_codec_slots[0].handle=&device_token;g_codec_slots[0].owner_session_id=1;
    g_accept_operations=false;AudioCodecHandle denied=&device_token;
    CHECK(RemoveAudioCodec({1,AudioActivity::kCapture},&denied)==ESP_ERR_INVALID_STATE && removes==0);
#ifdef FIXTURE_ADMITTED_CLOSE
    ResourceSlot* close_slot=nullptr;
    CHECK(BeginCloseResource(g_codec_slots,2,{2,AudioActivity::kCapture},denied,&close_slot,true)==ESP_ERR_INVALID_STATE);
#endif
    Clear();registry_fails=true;
    CHECK(Configure()==ESP_FAIL && adds==2 && removes==2 && g_codec_slots[0].handle==nullptr);
    Clear();remove_fails=true;
    CHECK(Configure()==ESP_FAIL && !g_codec_configuration_valid && g_codec_slots[0].handle!=nullptr);
    remove_fails=false;
    AudioCodecHandle dev=&device_token;
    CHECK(RemoveAudioCodec({1,AudioActivity::kCapture},&dev)==ESP_OK && dev==nullptr);
    Clear();g_codec_slots[0].handle=&device_token;g_codec_slots[0].owner_session_id=1;
    standalone=true;fail_at=1;
    CHECK(WriteAudioCodecRegister({1,AudioActivity::kCapture},&device_token,0x16,0x24)==ESP_OK);
    CHECK(transfers==2 && mutex.depth==0);
    uint8_t value=0;fail_at=transfers+1;expected_deadline=clock_us+kCallTimeoutUs;
    CHECK(ReadAudioCodecRegister({1,AudioActivity::kCapture},&device_token,0x16,&value)==ESP_OK && value==0x24);
    Clear();g_codec_slots[0].handle=&device_token;g_codec_slots[0].owner_session_id=1;
    standalone=deny_locks=true;
    CHECK(WriteAudioCodecRegister({1,AudioActivity::kCapture},&device_token,0x16,0x24)==ESP_ERR_TIMEOUT);
    CHECK(transfers==0 && active_ops==0 && mutex.depth==0 && denials==10);
}
#endif
int main()
{
    Happy();
#ifndef FIXTURE_HAPPY_ONLY
    Faults();
#endif
    std::printf("Full codec budget %d Hz: %u checks PASS\n",configTICK_RATE_HZ,checks);
}
