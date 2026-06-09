idf_component_register(
    SRCS
        "main.cpp"
        "ai_session.cpp"
        "audio_capture.cpp"
        "audio_selftest.cpp"
        "board_zectrix_note4.cpp"
        "button_input.cpp"
        "contract_fixtures.cpp"
        "device_ui.cpp"
        "diagnostics.cpp"
        "epd_display.cpp"
        "fonts/SourceHanSansSC_Regular_slim.c"
        "power_manager.cpp"
        "storage.cpp"
        "text_render.cpp"
        "time_app.cpp"
        "ui_model.cpp"
        "wifi_manager.cpp"
        "word_pack.cpp"
        "word_app.cpp"
        "wqn_api.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_hw_support
        esp_system
        esp_timer
        app_update
        esp_adc
        esp_http_client
        esp-tls
        esp_event
        esp_netif
        esp_psram
        esp_wifi
        json
        lwip
        mbedtls
        nvs_flash
        spi_flash
        spiffs
)

set(WQN_API_BASE "" CACHE STRING "Override the WQN ESP32 API base URL")
set(WQN_DEBUG_PROBLEM_IDS "" CACHE STRING "Local-only comma-separated ESP32 debug problem IDs")

target_compile_definitions(
    ${COMPONENT_LIB}
    PRIVATE
        BOARD_TYPE=\"zectrix-s3-epaper-4.2\"
        BOARD_NAME=\"ZecTrix_Note4\"
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
