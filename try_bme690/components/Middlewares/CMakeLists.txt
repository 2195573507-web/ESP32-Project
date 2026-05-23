set(src_dirs
            wifi
            http_client
            llm_client
            lcd
            bme690)

set(include_dirs
            wifi
            http_client
            llm_client
            lcd
            bme690)

set(requires
            driver
            esp_wifi
            esp_event
            esp_netif
            nvs_flash
            log
            esp_http_client
            esp-tls
            jbrilha__esp_lcd_st7789
            esp_lcd
            espressif__bme690
            espressif__i2c_bus
            BSP)

idf_component_register(SRC_DIRS ${src_dirs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})
component_compile_options(-ffast-math -O3 -Wno-error=format -Wno-format)


    
