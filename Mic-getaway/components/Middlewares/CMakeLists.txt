set(srcs
            wifi/wifi_manager.c
            llm/llm_client.c
            llm/ai_bridge_common.c
            llm/volc_gateway_auth.c
            llm/llm_gateway_http.c
            llm/llm_gateway_ws.c
            llm/llm_gateway_tts_ws.c
            llm/llm_gateway_protocol.c
            mic/mic_adc_test.c
            mic/mic_adc_pcm.c
            mic/mic_record_buffer.c
            mic/mic_serial_output.c
            mic/mic_vad.c
            mic/mic_llm_bridge.c
            screen/ai_screen_bridge.c
            speaker/speaker_llm_bridge.c
            sensors/bme690_llm_bridge.c
            sensors/csi_llm_bridge.c
            system/system_llm_bridge.c)

set(include_dirs
            wifi
            llm
            mic
            screen
            speaker
            sensors
            system)

set(requires
            esp_wifi
            esp_event
            esp_netif
            nvs_flash
            esp_adc
            esp_http_client
            esp-tls
            mbedtls
            app_config
            json
            esp_rom
            esp_timer
            espressif__esp_websocket_client
            freertos
            log)

idf_component_register(SRCS ${srcs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})
component_compile_options(-Wno-error=format -Wno-format)
