idf_component_register(
    SRCS "manual_ws.c"
    INCLUDE_DIRS "."
    REQUIRES app_config esp-tls mbedtls log esp_system lwip
)
