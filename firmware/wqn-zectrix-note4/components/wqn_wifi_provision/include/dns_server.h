#pragma once

#include <atomic>

#include "esp_err.h"
#include "esp_netif_ip_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

namespace wqn::provision {

class DnsServer {
public:
    DnsServer();
    ~DnsServer();

    DnsServer(const DnsServer&) = delete;
    DnsServer& operator=(const DnsServer&) = delete;

    esp_err_t Start(esp_ip4_addr_t gateway);
    esp_err_t Stop(TickType_t timeout = pdMS_TO_TICKS(2000));

private:
    static void TaskEntry(void* arg);
    void Run();

    static constexpr EventBits_t kReadyBit = BIT0;
    static constexpr EventBits_t kFailedBit = BIT1;
    static constexpr EventBits_t kStoppedBit = BIT2;

    esp_ip4_addr_t gateway_ = {};
    EventGroupHandle_t events_ = nullptr;
    std::atomic<bool> running_{false};
};

}  // namespace wqn::provision
