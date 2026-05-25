set(srcs
            wifi/wifi_manager.c
            mic/mic_adc_test.c
            mic/mic_adc_pcm.c
            mic/mic_serial_output.c
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
            freertos
            log
            lwip)

idf_component_register(SRCS ${srcs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})
component_compile_options(-Wno-error=format -Wno-format)
