idf_component_register(
    SRCS
        "runtime/sleep_coordinator.cpp"
        "runtime/sleep_diagnostics.cpp"
        "runtime/sleep_snapshot.cpp"
        "runtime/wake_context.cpp"
        "power/sleep_protocol.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_hw_support
        esp_pm
        esp_timer
        freertos
)
