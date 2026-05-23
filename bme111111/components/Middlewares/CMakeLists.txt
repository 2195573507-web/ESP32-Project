idf_component_register(
    SRCS "bme690/bme690.c"
    INCLUDE_DIRS "bme690"
    REQUIRES BSP espressif__bme690 freertos log
)
