idf_component_register(
    SRCS
        "main.cpp"
        "ai_history.cpp"
        "ai_session.cpp"
        "audio_capture.cpp"
        "audio_player.cpp"
        "audio_sleep.cpp"
        "audio_volume.cpp"
        "audio_selftest.cpp"
        "board_zectrix_note4.cpp"
        "button_input.cpp"
        "contract_fixtures.cpp"
        "device_ui.cpp"
        "device_protocol/claim_crypto.cpp"
        "device_protocol/v3.cpp"
        "diagnostics.cpp"
        "epd_display.cpp"
        "fonts/SourceHanSansSC_Regular_slim.c"
        "fonts/font_zectrix_16_1.c"
        "fonts/font_zectrix_48_1.c"
        "power_manager.cpp"
        "power/sleep_protocol.cpp"
        "power/wake_controller.cpp"
        "pcf8563.cpp"
        "provision_manager.cpp"
        "runtime/sleep_coordinator.cpp"
        "runtime/sleep_snapshot.cpp"
        "runtime/storage_schema.cpp"
        "runtime/wake_context.cpp"
        "services/connectivity_service.cpp"
        "services/audio_service.cpp"
        "services/storage_service.cpp"
        "services/sync_service.cpp"
        "sse_chunk.cpp"
        "storage.cpp"
        "text_render.cpp"
        "time_app.cpp"
        "ui/battery.cpp"
        "ui/clock.cpp"
        "ui/diagnostics.cpp"
        "ui/graphics.cpp"
        "ui/page_ai.cpp"
        "ui/page_home.cpp"
        "ui/page_settings.cpp"
        "ui/page_time.cpp"
        "ui/page_todo.cpp"
        "ui/page_word.cpp"
        "ui/seven_segment.cpp"
        "ui/text_wrappers.cpp"
        "ui/todo_cloud.cpp"
        "ui/ui_input.cpp"
        "ui/ui_refresh.cpp"
        "ui/ui_render.cpp"
        "ui/ui_runtime.cpp"
        "ui/ui_state.cpp"
        "ui/word_cloud.cpp"
        "ui_model.cpp"
        "wifi_manager.cpp"
        "word_pack.cpp"
        "word_app.cpp"
        "wqn_api.cpp"
        "wqn_api_stream.cpp"
        "flash_session.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_hw_support
        esp_http_server
        esp_system
        esp_timer
        app_update
        esp_adc
        esp_http_client
        esp_websocket_client
        esp_event
        esp_netif
        esp_psram
        esp_wifi
        wqn_wifi_provision
        freertos
        json
        lwip
        mbedtls
        nvs_flash
        spi_flash
        spiffs
)

set(WQN_DEVICE_CONTROL_CONTRACT_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../contracts/device-control-v3")
set(WQN_DEVICE_CONTROL_SCHEMA
    "${WQN_DEVICE_CONTROL_CONTRACT_DIR}/device-control-v3.schema.json")
set(WQN_DEVICE_CONTROL_MANIFEST
    "${WQN_DEVICE_CONTROL_CONTRACT_DIR}/manifest.json")
file(SHA256 "${WQN_DEVICE_CONTROL_SCHEMA}" WQN_DEVICE_CONTROL_SCHEMA_SHA256)
file(READ "${WQN_DEVICE_CONTROL_MANIFEST}" WQN_DEVICE_CONTROL_MANIFEST_JSON)
string(JSON WQN_DEVICE_CONTROL_MANIFEST_SHA256
    GET "${WQN_DEVICE_CONTROL_MANIFEST_JSON}" schema_sha256)
if(NOT WQN_DEVICE_CONTROL_SCHEMA_SHA256 STREQUAL WQN_DEVICE_CONTROL_MANIFEST_SHA256)
    message(FATAL_ERROR
        "device-control-v3 schema hash mismatch: actual=${WQN_DEVICE_CONTROL_SCHEMA_SHA256} "
        "manifest=${WQN_DEVICE_CONTROL_MANIFEST_SHA256}")
endif()

# M6 ownership gate: feature/session modules only see opaque AudioService
# handles. Note4 bootstrap and AudioService are the two deliberate low-level
# exceptions (pre-service safe pins, then the sole runtime owner).
set(WQN_AUDIO_CLIENT_SOURCES
    "audio_capture.cpp"
    "audio_player.cpp"
    "audio_selftest.cpp"
    "audio_sleep.cpp"
    "audio_volume.cpp"
    "flash_session.cpp")
foreach(WQN_AUDIO_CLIENT IN LISTS WQN_AUDIO_CLIENT_SOURCES)
    file(READ "${CMAKE_CURRENT_LIST_DIR}/${WQN_AUDIO_CLIENT}"
        WQN_AUDIO_CLIENT_SOURCE)
    if(WQN_AUDIO_CLIENT_SOURCE MATCHES
        "#[ \t]*include[ \t]*[<\"]driver/(gpio|i2c_master|i2s_std)\\.h")
        message(FATAL_ERROR
            "M6 audio ownership violation: ${WQN_AUDIO_CLIENT} includes an ESP-IDF audio driver")
    endif()
    if(WQN_AUDIO_CLIENT_SOURCE MATCHES
        "GPIO_NUM_(42|46)|GetSharedI2cBusHandle")
        message(FATAL_ERROR
            "M6 audio ownership violation: ${WQN_AUDIO_CLIENT} bypasses AudioService")
    endif()
endforeach()

set(WQN_API_BASE "" CACHE STRING "Override the WQN ESP32 API base URL")
set(WQN_DEBUG_PROBLEM_IDS "" CACHE STRING "Local-only comma-separated ESP32 debug problem IDs")

target_compile_definitions(
    ${COMPONENT_LIB}
    PRIVATE
        BOARD_TYPE=\"zectrix-s3-epaper-4.2\"
        BOARD_NAME=\"ZecTrix_Note4\"
        WQN_DEVICE_CONTROL_SCHEMA_SHA256=\"${WQN_DEVICE_CONTROL_SCHEMA_SHA256}\"
)

if(WQN_API_BASE)
    target_compile_definitions(
        ${COMPONENT_LIB}
        PRIVATE
            WQN_API_BASE=\"${WQN_API_BASE}\"
    )
endif()

if(WQN_DEBUG_PROBLEM_IDS)
    target_compile_definitions(
        ${COMPONENT_LIB}
        PRIVATE
            WQN_DEBUG_PROBLEM_IDS=\"${WQN_DEBUG_PROBLEM_IDS}\"
    )
endif()
