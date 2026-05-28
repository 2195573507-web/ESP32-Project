idf_component_register(
    SRCS "manual_ws.c"
    INCLUDE_DIRS "."
    REQUIRES esp-tls mbedtls log esp_system lwip
)
