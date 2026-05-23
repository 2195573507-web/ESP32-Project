set(srcs
            wifi/wifi_manager.c
            csi/csi_monitor.c
            csi/csi_serial_output.c
            csi/csi_processor.c
            csi/csi_subband_analysis.c
            csi/csi_gain_compensation.c)

set(include_dirs
            wifi
            csi)

set(requires
            esp_wifi
            esp_event
            esp_netif
            nvs_flash
            esp_timer
            log
            lwip
            espressif__esp_csi_gain_ctrl)

idf_component_register(SRCS ${srcs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})
component_compile_options(-ffast-math -O3 -Wno-error=format -Wno-format)


    
