set(srcs
            wifi/wifi_manager.c
            llm/http_client.c
            llm/llm_client.c
            llm/mic_llm_doubao.c
            mic/mic_asr_llm_bridge.c
            mic/mic_adc_test.c
            mic/mic_adc_pcm.c
            mic/mic_asr_doubao.c
            mic/mic_asr_doubao_parser.c
            mic/mic_vad.c)

set(include_dirs
            wifi
            llm
            mic)

set(requires
            esp_wifi
            esp_event
            esp_netif
            nvs_flash
            esp_adc
            esp_http_client
            mbedtls
            app_config
            manual_ws
            json
            esp_rom
            freertos
            log)

idf_component_register(SRCS ${srcs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})
component_compile_options(-Wno-error=format -Wno-format)
