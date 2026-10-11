idf_component_register(
    SRCS
        "display_service.cpp"
        "fonts/SourceHanSansSC_Regular_slim.c"
        "fonts/font_zectrix_16_1.c"
        "fonts/font_zectrix_48_1.c"
    INCLUDE_DIRS "include" "private_include"
    REQUIRES
        driver
        esp_hw_support
        esp_system
        esp_timer
        freertos
        power_runtime
)
