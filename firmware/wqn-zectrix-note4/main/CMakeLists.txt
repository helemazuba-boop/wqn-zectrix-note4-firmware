idf_component_register(
    SRCS
        "main.cpp"
        "ai_feature.cpp"
        "ai_history.cpp"
        "ai_session.cpp"
        "audio_capture.cpp"
        "audio_player.cpp"
        "audio_selftest.cpp"
        "button_input.cpp"
        "contract_fixtures.cpp"
        "device_ui.cpp"
        "diagnostics.cpp"
        "error_recorder.cpp"
        "i2c_bus_lock.cpp"
        "power_manager.cpp"
        "power/rtc_timekeep.cpp"
        "power/wake_controller.cpp"
        "pcf8563.cpp"
        "provision_manager.cpp"
        "runtime/storage_schema.cpp"
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
        "ui/cloud_runner.cpp"
        "ui/persist_worker.cpp"
        "ui/diagnostics.cpp"
        "ui/graphics.cpp"
        "ui/markdown_layout.cpp"
        "ui/markdown_render.cpp"
        "ui/page_ai.cpp"
        "ui/page_agent.cpp"
        "ui/page_home.cpp"
        "ui/page_settings.cpp"
        "ui/page_time.cpp"
        "ui/page_todo.cpp"
        "ui/page_word.cpp"
        "ui/seven_segment.cpp"
        "ui/text_wrappers.cpp"
        "ui/time_app_retention.cpp"
        "ui/todo_cloud.cpp"
        "ui/note_cloud.cpp"
        "ui/page_note.cpp"
        "ui/problem_cloud.cpp"
        "ui/page_problem.cpp"
        "ui/ui_input.cpp"
        "ui/ui_refresh.cpp"
        "ui/ui_render.cpp"
        "ui/ui_runtime.cpp"
        "ui/ui_state.cpp"
        "ui/ui_widgets.cpp"
        "ui/word_cloud.cpp"
        "ui_model.cpp"
        "wifi_manager.cpp"
        "note_pack.cpp"
        "note_cloud.cpp"
        "note_store.cpp"
        "note_app.cpp"
        "opencode_client.cpp"
        "opencode_session.cpp"
        "problem_pack.cpp"
        "problem_cloud.cpp"
        "problem_store.cpp"
        "problem_app.cpp"
        "word_pack.cpp"
        "word_app.cpp"
        "word_study_store.cpp"
        "wqn_api.cpp"
        "wqn_api_stream.cpp"
        "flash_session.cpp"
        "stdpro_ws_transport.cpp"
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
        esp_rom
        esp_driver_usb_serial_jtag
        esp_wifi
        wqn_wifi_provision
        freertos
        json
        lwip
        mbedtls
        nvs_flash
        platform_note4
        power_runtime
        display_service
        device_protocol
        spi_flash
        spiffs
)

if(CONFIG_WQN_WIFI_STA_ENABLE AND NOT CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE)
    message(FATAL_ERROR
        "WiFi-enabled firmware must use device-control v3; legacy /poll pairing returns HTTP 426")
endif()

set(WQN_API_BASE "" CACHE STRING "Override the WQN ESP32 API base URL")

# [dev-diag] Build identity for the Dev info dialog (DEV_DIAGNOSTICS.md §4.1).
# Both values are configure-time snapshots: a commit made after configure only
# shows up on the next reconfigure. Git absence (building from an export)
# degrades to "unknown". Deliberately separate from WQN_FIRMWARE_VERSION --
# that string feeds the sync image_id and the EPD frame signature and must not
# change on every commit.
execute_process(
    COMMAND git -C ${CMAKE_CURRENT_LIST_DIR}/../.. describe --always --dirty --abbrev=8
    OUTPUT_VARIABLE WQN_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE WQN_GIT_COMMIT_RESULT
)
if(NOT WQN_GIT_COMMIT_RESULT EQUAL 0 OR NOT WQN_GIT_COMMIT)
    set(WQN_GIT_COMMIT "unknown")
endif()
# The release tooling pins this to the version's original timestamp so both
# machines report the same build time for the same version.
if(DEFINED ENV{WQN_BUILD_TIME} AND NOT "$ENV{WQN_BUILD_TIME}" STREQUAL "")
    set(WQN_BUILD_TIME "$ENV{WQN_BUILD_TIME}")
else()
    string(TIMESTAMP WQN_BUILD_TIME "%Y-%m-%d %H:%M" UTC)
endif()

# [release] Single source of truth for the firmware version: PROJECT_VER is set
# in the top-level CMakeLists.txt from WQN_RELEASE_VERSION. It feeds the sync
# image_id and the EPD frame signature, so it must not drift from the release
# record. main/config.h keeps an #ifndef fallback for standalone parsers.
idf_build_get_property(WQN_PROJECT_VER PROJECT_VER)

target_compile_definitions(
    ${COMPONENT_LIB}
    PRIVATE
        BOARD_TYPE=\"zectrix-s3-epaper-4.2\"
        BOARD_NAME=\"ZecTrix_Note4\"
        WQN_GIT_COMMIT=\"${WQN_GIT_COMMIT}\"
        WQN_BUILD_TIME=\"${WQN_BUILD_TIME}\"
        WQN_FIRMWARE_VERSION=\"${WQN_PROJECT_VER}\"
)

if(WQN_API_BASE)
    target_compile_definitions(
        ${COMPONENT_LIB}
        PRIVATE
            WQN_API_BASE=\"${WQN_API_BASE}\"
    )
endif()
