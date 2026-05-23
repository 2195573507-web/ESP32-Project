idf_component_register(
                    SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES Middlewares esp_wifi esp_event esp_netif nvs_flash log
)
