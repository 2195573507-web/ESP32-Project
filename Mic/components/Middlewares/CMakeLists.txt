set(srcs
            wifi/wifi_manager.c
            mic/mic_adc_test.c
            mic/mic_adc_pcm.c
            mic/mic_asr_doubao.c
            mic/mic_asr_doubao_parser.c
            mic/mic_network_diag.c
            mic/mic_vad.c)

set(include_dirs
            wifi
            mic)

set(requires
            esp_wifi
            esp_event
            esp_netif
            nvs_flash
            esp_adc
            manual_ws
            esp-tls
            json
            mbedtls
            freertos
            log
            lwip)

idf_component_register(SRCS ${srcs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})
component_compile_options(-Wno-error=format -Wno-format)
