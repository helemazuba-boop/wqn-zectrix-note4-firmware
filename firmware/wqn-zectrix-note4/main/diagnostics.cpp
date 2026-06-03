#include "diagnostics.h"

#include <array>
#include <cstdint>

#include "config.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "esp_system.h"

namespace {

constexpr char kTag[] = "wqn_diag";

const char* ResetReasonToString(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_POWERON:
            return "poweron";
        case ESP_RST_EXT:
            return "external";
        case ESP_RST_SW:
            return "software";
        case ESP_RST_PANIC:
            return "panic";
        case ESP_RST_INT_WDT:
            return "interrupt watchdog";
        case ESP_RST_TASK_WDT:
            return "task watchdog";
        case ESP_RST_WDT:
            return "other watchdog";
        case ESP_RST_DEEPSLEEP:
            return "deep sleep";
        case ESP_RST_BROWNOUT:
            return "brownout";
        case ESP_RST_SDIO:
            return "sdio";
        case ESP_RST_USB:
            return "usb";
        case ESP_RST_JTAG:
            return "jtag";
        case ESP_RST_EFUSE:
            return "efuse";
        case ESP_RST_PWR_GLITCH:
            return "power glitch";
        case ESP_RST_CPU_LOCKUP:
            return "cpu lockup";
        default:
            return "unknown";
    }
}

}  // namespace

namespace wqn {

void PrintBootDiagnostics()
{
    esp_chip_info_t chip_info = {};
    esp_chip_info(&chip_info);

    uint32_t flash_size = 0;
    const esp_err_t flash_result = esp_flash_get_size(nullptr, &flash_size);

    std::array<uint8_t, 6> mac = {};
    const esp_err_t mac_result = esp_read_mac(mac.data(), ESP_MAC_WIFI_STA);

    ESP_LOGI(kTag, "%s %s", WQN_FIRMWARE_NAME, WQN_FIRMWARE_VERSION);
    ESP_LOGI(kTag, "board=%s target=%s idf=%s", WQN_BOARD_ID, CONFIG_IDF_TARGET, IDF_VER);
    ESP_LOGI(kTag, "api_base=%s", WQN_API_BASE);
    ESP_LOGI(
        kTag,
        "chip cores=%d revision=%d features=%s%s%s",
        chip_info.cores,
        chip_info.revision,
        (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "wifi " : "",
        (chip_info.features & CHIP_FEATURE_BLE) ? "ble " : "",
        (chip_info.features & CHIP_FEATURE_EMB_PSRAM) ? "psram" : "");

    if (flash_result == ESP_OK) {
        ESP_LOGI(kTag, "flash_size=%lu bytes", static_cast<unsigned long>(flash_size));
    } else {
        ESP_LOGW(kTag, "flash size unavailable: %s", esp_err_to_name(flash_result));
    }

    if (esp_psram_is_initialized()) {
        ESP_LOGI(kTag, "psram=initialized size=%lu bytes", static_cast<unsigned long>(esp_psram_get_size()));
    } else {
        ESP_LOGW(kTag, "psram=not initialized");
    }

    if (mac_result == ESP_OK) {
        ESP_LOGI(
            kTag,
            "wifi_mac=%02X:%02X:%02X:%02X:%02X:%02X",
            mac[0],
            mac[1],
            mac[2],
            mac[3],
            mac[4],
            mac[5]);
    } else {
        ESP_LOGW(kTag, "wifi mac unavailable: %s", esp_err_to_name(mac_result));
    }

    ESP_LOGI(kTag, "reset_reason=%s", ResetReasonToString(esp_reset_reason()));
}

}  // namespace wqn
