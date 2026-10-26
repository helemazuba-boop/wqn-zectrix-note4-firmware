#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "pcf8563.h"
#include "power/wake_controller.h"
#include "runtime/wake_context.h"
#include "runtime/task_delay.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); std::exit(1); } } while (0)

constexpr int64_t kTickUs = 1000000 / configTICK_RATE_HZ;
static int64_t monotonic_us = 1000000, deadline, lock_cost_us, transfer_cost_us;
static int64_t busy_low_until, late_delay_us;
static unsigned transfers, takes, gives, fail_transfer, late_transfer, fail_lock;
static unsigned commands, fail_command, gpio_reads, late_gpio_read;
static unsigned ext1_calls, timer_calls, light_calls, key_wake_calls, feed_calls;
static bool fail_ext1, fail_timer, late_ext1, late_timer, rtc_active_after_arm;
static bool light_enabled;
static uint64_t armed_mask, timer_us;
static std::array<int, 19> pins;
static std::array<uint8_t, 256> registers;
static std::vector<int> timeouts;
static std::vector<int64_t> waits;
struct FixtureMutex { unsigned depth = 0; };
static FixtureMutex mutex;
static int bus_token, rtc_token;

int64_t esp_timer_get_time() { return monotonic_us; }
TickType_t xTaskGetTickCount() { return static_cast<TickType_t>(monotonic_us / kTickUs); }
void FixtureLog(const char*, const char*, ...) {}
namespace wqn { void RecordError(const char*, const char*, ...) {} }
namespace wqn::runtime {
void CaptureWakeContext(uint64_t, uint64_t, bool, bool, bool) {}
}
void vTaskDelay(TickType_t ticks)
{
    CHECK(ticks > 0 && ticks < portMAX_DELAY);
    // Worst phase for a short wait: the next tick can arrive immediately.
    const int64_t elapsed = int64_t{ticks} * kTickUs - monotonic_us % kTickUs;
    waits.push_back(elapsed);
    monotonic_us += elapsed + late_delay_us;
}
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() { return &mutex; }
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t value, TickType_t ticks)
{
    CHECK(value == &mutex && ticks <= pdMS_TO_TICKS(100));
    if (deadline > 0) {
        CHECK(monotonic_us < deadline);
        CHECK(int64_t{ticks} * kTickUs <= deadline - monotonic_us);
    }
    ++takes;
    monotonic_us += lock_cost_us;
    if (takes == fail_lock) return pdFALSE;
    ++mutex.depth;
    return pdTRUE;
}
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t value)
{
    CHECK(value == &mutex && mutex.depth > 0);
    --mutex.depth; ++gives;
    return pdTRUE;
}
esp_err_t i2c_master_bus_add_device(void* bus, const i2c_device_config_t* cfg, void** dev)
{
    CHECK(bus == &bus_token && cfg->device_address == 0x51);
    *dev = &rtc_token;
    return ESP_OK;
}
static bool Transfer(void* dev, int timeout)
{
    CHECK(dev == &rtc_token && mutex.depth == 1);
    CHECK(timeout > 0 && timeout <= 100);
    if (deadline > 0) {
        CHECK(monotonic_us < deadline);
        CHECK(int64_t{timeout} * 1000 <= deadline - monotonic_us);
    }
    timeouts.push_back(timeout);
    ++transfers;
    monotonic_us += transfer_cost_us;
    if (transfers == late_transfer) monotonic_us = deadline;
    return transfers != fail_transfer;
}
esp_err_t i2c_master_transmit(void* dev, const uint8_t* bytes, size_t size, int timeout)
{
    CHECK(size >= 2 && size_t{bytes[0]} + size - 1 <= registers.size());
    if (!Transfer(dev, timeout)) return ESP_ERR_INVALID_STATE;
    std::memcpy(registers.data() + bytes[0], bytes + 1, size - 1);
    return ESP_OK;
}
esp_err_t i2c_master_transmit_receive(void* dev, const uint8_t* reg, size_t write_size,
                                    uint8_t* out, size_t size, int timeout)
{
    CHECK(write_size == 1 && size_t{*reg} + size <= registers.size());
    if (!Transfer(dev, timeout)) return ESP_ERR_INVALID_STATE;
    std::memcpy(out, registers.data() + *reg, size);
    return ESP_OK;
}
int gpio_get_level(gpio_num_t pin)
{
    if (pin == GPIO_NUM_8) return monotonic_us >= busy_low_until;
    ++gpio_reads;
    if (gpio_reads == late_gpio_read) monotonic_us = deadline;
    if (pin == GPIO_NUM_5 && rtc_active_after_arm && (registers[14] & 0x80)) return 0;
    return pins[pin];
}
esp_err_t gpio_wakeup_enable(gpio_num_t pin, int)
{
    CHECK(pin == GPIO_NUM_0 || pin == GPIO_NUM_18);
    ++key_wake_calls;
    return ESP_OK;
}
esp_err_t esp_sleep_enable_gpio_wakeup() { light_enabled = true; ++light_calls; return ESP_OK; }
esp_err_t esp_sleep_disable_wakeup_source(esp_sleep_wakeup_cause_t cause)
{
    if (cause == ESP_SLEEP_WAKEUP_EXT1) armed_mask = 0;
    else if (cause == ESP_SLEEP_WAKEUP_TIMER) timer_us = 0;
    else CHECK(false);
    return ESP_OK;
}
esp_err_t esp_sleep_enable_ext1_wakeup(uint64_t mask, int mode)
{
    CHECK(mode == ESP_EXT1_WAKEUP_ANY_LOW);
    ++ext1_calls;
    if (late_ext1) monotonic_us = deadline;
    if (fail_ext1) return ESP_FAIL;
    armed_mask = mask;
    return ESP_OK;
}
esp_err_t esp_sleep_enable_timer_wakeup(uint64_t us)
{
    ++timer_calls;
    if (late_timer) monotonic_us = deadline;
    if (fail_timer) return ESP_FAIL;
    timer_us = us;
    return ESP_OK;
}

// Run the complete production BUSY/status functions with GPIO/command ports.
constexpr char kTag[] = "host_epd";
namespace runtime = wqn::runtime;
constexpr gpio_num_t kEpdBusy = GPIO_NUM_8;
void FeedTwdtIfSubscribed() { ++feed_calls; }
esp_err_t SendCommand(uint8_t) { return ++commands == fail_command ? ESP_FAIL : ESP_OK; }
esp_err_t SendData(uint8_t) { return ++commands == fail_command ? ESP_FAIL : ESP_OK; }
#define ESP_RETURN_ON_ERROR(expr,...) do { auto r=(expr); if (r!=ESP_OK) return r; } while (0)
@@EPD_BUSY@@
@@EPD_STATUS@@

static void Reset(bool rtc = true)
{
    CHECK(mutex.depth == 0);
    monotonic_us = 1000000 + kTickUs - 1;
    deadline = lock_cost_us = transfer_cost_us = busy_low_until = late_delay_us = 0;
    transfers = takes = gives = fail_transfer = late_transfer = fail_lock = 0;
    ext1_calls = timer_calls = light_calls = key_wake_calls = feed_calls = 0;
    gpio_reads = late_gpio_read = commands = fail_command = 0;
    fail_ext1 = fail_timer = late_ext1 = late_timer = rtc_active_after_arm = false;
    armed_mask = timer_us = 0;
    light_enabled = false;
    pins.fill(1); registers.fill(0); timeouts.clear(); waits.clear();
    wqn::power::SetPcf8563WakeAvailable(rtc);
}
static wqn::power::WakeArmResult Arm(uint32_t seconds = 60, int64_t budget_us = 2000000)
{
    deadline = monotonic_us + budget_us;
    auto result = wqn::power::ArmWakeSources(seconds, deadline);
    CHECK(light_enabled && key_wake_calls >= 2 && mutex.depth == 0 && takes == gives);
    return result;
}
static void WakeCases()
{
    using namespace wqn::power;
    CHECK(wqn::Pcf8563InitWithBus(&bus_token));
    Reset(); transfer_cost_us = 1000;
    auto result = Arm();
    CHECK(result.error == ESP_OK && result.timer_source == TimerWakeSource::kPcf8563);
    CHECK(transfers == 9 && ext1_calls == 1 && timer_calls == 0);
    CHECK(armed_mask == ((1ULL << 0) | (1ULL << 18) | (1ULL << 5) | (1ULL << 2) | (1ULL << 1)));
    CHECK(waits.size() == 2 && waits[0] >= 2000 && waits[1] >= 2000);
    Reset(false);
    result = Arm();
    CHECK(result.error == ESP_OK && result.timer_source == TimerWakeSource::kEsp32);
    CHECK(transfers == 0 && timer_us == 60000000);
    Reset();
    result = Arm(300);
    CHECK(result.error == ESP_OK && result.timer_source == TimerWakeSource::kEsp32);
    CHECK(transfers == 6 && timer_us == 300000000);
    Reset();
    result = Arm(0);
    CHECK(result.error == ESP_OK && result.timer_source == TimerWakeSource::kDisabled);
    CHECK(transfers == 3 && timer_calls == 0);
    Reset(); fail_transfer = 4;
    result = Arm();
    CHECK(result.error == ESP_OK && result.timer_source == TimerWakeSource::kEsp32);
    CHECK(transfers == 7 && timer_calls == 1);
    Reset(); fail_transfer = 1;
    CHECK(Arm().error == ESP_FAIL && transfers == 1 && ext1_calls == 0);
    Reset(false); fail_timer = true;
    CHECK(Arm().error == ESP_FAIL && ext1_calls == 0 && timer_us == 0);
    Reset(); fail_ext1 = true;
    CHECK(Arm().error == ESP_FAIL && armed_mask == 0 && registers[1] == 0);
    Reset(); pins[GPIO_NUM_0] = 0;
    result = Arm();
    CHECK(result.error == ESP_ERR_INVALID_STATE && result.active_gpio_mask == 1);
    CHECK(ext1_calls == 0 && transfers == 3);
    Reset(); pins[GPIO_NUM_1] = 0;
    result = Arm();
    CHECK(result.error == ESP_OK && !(armed_mask & (1ULL << 1)));
    Reset(); rtc_active_after_arm = true;
    result = Arm();
    CHECK(result.error == ESP_ERR_INVALID_STATE && result.active_gpio_mask == (1ULL << 5));
    CHECK(armed_mask == 0 && registers[1] == 0);

    Reset();
    CHECK(Arm(60, -1).error == ESP_ERR_TIMEOUT && transfers == 0 && waits.empty());
    Reset();
    CHECK(Arm(60, 1000).error == ESP_ERR_TIMEOUT && ext1_calls == 0 && waits.empty());
    // Late success at every initial clear/configure step must end the round.
    for (unsigned step = 1; step <= 9; ++step) {
        Reset(); late_transfer = step;
        result = Arm();
        CHECK(result.error == ESP_ERR_TIMEOUT && transfers == step);
        CHECK(ext1_calls == 0 && timer_calls == 0 && armed_mask == 0);
    }
    Reset(); lock_cost_us = 2000000;
    CHECK(Arm().error == ESP_ERR_TIMEOUT && transfers == 0 && takes == 1);
    Reset(); late_delay_us = 2000000;
    CHECK(Arm().error == ESP_ERR_TIMEOUT && ext1_calls == 0);
    Reset(); late_ext1 = true;
    CHECK(Arm().error == ESP_ERR_TIMEOUT && ext1_calls == 1 && armed_mask == 0);
    Reset(false); late_timer = true;
    CHECK(Arm().error == ESP_ERR_TIMEOUT && timer_us == 0 && ext1_calls == 0);
    Reset(); late_gpio_read = 3;
    CHECK(Arm().error == ESP_ERR_TIMEOUT && ext1_calls == 0);
    Reset(); transfer_cost_us = 1000;
    CHECK(Arm(60, 75000).error == ESP_OK);
    CHECK(timeouts.size() == 9 && timeouts.front() <= 75 && timeouts.back() < timeouts.front());
}

static void RtcBudgetCases()
{
    for (unsigned step = 1; step <= 6; ++step) {
        Reset(); deadline = monotonic_us + 50000; late_transfer = step;
        CHECK(!wqn::Pcf8563ConfigureTimerWake(60, deadline));
        CHECK(transfers == step && takes == gives);
    }
    for (unsigned step = 1; step <= 3; ++step) {
        Reset(); deadline = monotonic_us + 50000; late_transfer = step;
        CHECK(!wqn::Pcf8563DisableTimerWakeAndClearFlags(deadline));
        CHECK(transfers == step && takes == gives);
    }
    Reset(); deadline = monotonic_us - 1;
    CHECK(!wqn::Pcf8563ConfigureTimerWake(60, deadline) && takes == 0 && transfers == 0);
    Reset(); deadline = monotonic_us + 1500; transfer_cost_us = 1000;
    CHECK(!wqn::Pcf8563ConfigureTimerWake(60, deadline) && transfers == 1);
    CHECK(timeouts == std::vector<int>{1});
    Reset(); deadline = monotonic_us + 50000; fail_lock = 1;
    CHECK(!wqn::Pcf8563DisableTimerWakeAndClearFlags(deadline));
    CHECK(transfers == 0 && gives == 0);
    Reset();
    CHECK(wqn::Pcf8563ConfigureTimerWake(60));
    CHECK(transfers == 6 && timeouts == std::vector<int>(6, 100));
}

static void ShortWaitCases()
{
    for (int64_t phase : {int64_t{0}, kTickUs / 2, kTickUs - 1}) {
        Reset(); monotonic_us += phase - monotonic_us % kTickUs;
        const auto start = monotonic_us;
        wqn::runtime::DelayAtLeastMs<1>();
        CHECK(monotonic_us - start >= 1000);
        CHECK(monotonic_us - start <= 1000 + 2 * kTickUs);
    }
    Reset(); busy_low_until = monotonic_us + 2500;
    CHECK(WaitBusyTimeout(100) == ESP_OK && !waits.empty() && feed_calls > 0);
    Reset(); busy_low_until = monotonic_us + 1000000;
    CHECK(WaitBusyTimeout(5) == ESP_ERR_TIMEOUT && !waits.empty());
    Reset();
    auto start = monotonic_us;
    CHECK(WaitPanelStatusReady(3, 5) == ESP_OK && commands == 3);
    CHECK(monotonic_us - start >= 1000);
    Reset(); busy_low_until = monotonic_us + 1000000;
    CHECK(WaitPanelStatusReady(2, 1) == ESP_ERR_TIMEOUT && commands == 6);
    for (unsigned command = 1; command <= 3; ++command) {
        Reset(); fail_command = command;
        CHECK(WaitPanelStatusReady(3, 5) == ESP_FAIL && commands == command);
    }
}

int main()
{
    WakeCases(); RtcBudgetCases(); ShortWaitCases();
    std::printf("Wake/RTC deadline and short waits %d Hz: %u checks PASS\n", configTICK_RATE_HZ, checks);
}
