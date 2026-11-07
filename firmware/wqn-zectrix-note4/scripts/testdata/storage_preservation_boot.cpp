#include <cstdint>
#include <cstdio>
#include <string>
#include "runtime/storage_preservation_boot.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
std::string mode;
int64_t now=1000000;
unsigned reads=0,delays=0,checks=0,failures=0;
#define CHECK(x) do { ++checks; if(!(x)){++failures;std::printf("FAIL preservation-boot:%d: %s\n",__LINE__,#x);} } while(false)
esp_reset_reason_t esp_reset_reason() { return mode=="wake" ? ESP_RST_DEEPSLEEP : ESP_RST_SW; }
namespace wqn {
int ReadNote4BootPageUpLevel() {
    ++reads;
    if(mode=="idle")return 1;
    if(mode=="invalid-first" || (mode=="invalid-later" && reads>2))return -1;
    if(mode=="short" && now>=3990000)return 1;
    if(mode=="release" && reads>5)return 1;
    return 0;
}
}
void vTaskDelay(TickType_t ticks) {
    CHECK(ticks>0);++delays;
    if(mode!="stalled") now+=static_cast<int64_t>(ticks)*1000000/configTICK_RATE_HZ;
    if(mode=="reverse") now=999999;
}
int64_t esp_timer_get_time() { return mode=="negative" ? -1 : now; }
int main() {
    for(const char* name:{"idle","held","short","release","wake","invalid-first","invalid-later","stalled","reverse","negative"}) {
        mode=name; now=1000000;reads=delays=0;bool requested=true;
        const int error=wqn::runtime::CheckStoragePreservationBoot(&requested);
        const bool fault=mode=="invalid-first" || mode=="invalid-later" || mode=="stalled" || mode=="reverse" || mode=="negative";
        CHECK((error==ESP_OK)==!fault);
        CHECK(requested==(mode=="held"));
        if(mode=="held")CHECK(now>=4000000);
        if(mode=="idle" || mode=="wake" || mode=="invalid-first" || mode=="negative")CHECK(delays==0);
        if(mode=="wake")CHECK(reads==0);
        CHECK(delays<=512);
    }
    reads=delays=0;
    CHECK(wqn::runtime::CheckStoragePreservationBoot(nullptr)==ESP_ERR_INVALID_ARG);
    CHECK(reads==0 && delays==0);
    std::printf("Preservation boot: %u checks, %u failures (production selector; %d Hz; NOT HIL)\n",checks,failures,configTICK_RATE_HZ);
    return failures ? 1 : 0;
}
