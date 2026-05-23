idf_component_register(
    SRCS "I2C/iic.c"
    INCLUDE_DIRS "I2C"
    REQUIRES esp_driver_i2c esp_driver_gpio
)
