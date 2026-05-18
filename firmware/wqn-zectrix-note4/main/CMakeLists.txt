idf_component_register(
    SRCS
        "main.cpp"
        "board_zectrix_note4.cpp"
        "contract_fixtures.cpp"
        "diagnostics.cpp"
        "storage.cpp"
        "text_render.cpp"
        "wifi_manager.cpp"
        "wqn_api.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_hw_support
        esp_system
        esp_timer
        app_update
        esp_http_client
        esp-tls
        esp_event
        esp_netif
        esp_psram
        esp_wifi
        json
        lwip
        nvs_flash
        spi_flash
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
