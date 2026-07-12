idf_component_register(
    SRCS "board.c" "pcf8574.c" "onewire.c"
    INCLUDE_DIRS "include" "."
    REQUIRES driver esp_adc esp_timer
)
