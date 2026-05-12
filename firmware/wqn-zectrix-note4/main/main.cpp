#include <string>

#include "board_zectrix_note4.h"
#include "contract_fixtures.h"
#include "diagnostics.h"
#include "esp_ota_ops.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "storage.h"
#include "wifi_manager.h"

namespace {

constexpr char kTag[] = "wqn_main";

void LogTokenState()
{
    std::string token;
    const esp_err_t result = wqn::LoadAccessToken(&token);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "access token load failed: %s", esp_err_to_name(result));
        return;
    }

    if (token.empty()) {
        ESP_LOGI(kTag, "not paired: no stored access token");
    } else {
        ESP_LOGI(kTag, "paired token present: %s", wqn::MaskTokenForLog(token).c_str());
    }
}

void ConfirmRunningApp()
{
    const esp_err_t result = esp_ota_mark_app_valid_cancel_rollback();
    if (result == ESP_OK) {
        ESP_LOGI(kTag, "running app marked valid");
    } else {
        ESP_LOGW(kTag, "running app valid mark skipped: %s", esp_err_to_name(result));
    }
}

}  // namespace

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(wqn::InitZectrixNote4SafePins());
    wqn::PrintBootDiagnostics();

    ESP_ERROR_CHECK(wqn::InitStorage());
    LogTokenState();

    if (!wqn::RunContractFixtureSelfTest()) {
        ESP_LOGE(kTag, "contract fixture self-test failed; network flow remains disabled");
    }

    ConfirmRunningApp();
    ESP_ERROR_CHECK(wqn::StartWifiStationIfEnabled());

    ESP_LOGI(kTag, "headless initialization complete");
    ESP_LOGI(kTag, "network APIs remain disabled in this milestone");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
