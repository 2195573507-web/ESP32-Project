#include <stdio.h>

#include "bme690.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "APP";

void app_main(void)
{
    ESP_LOGI(TAG, "app start");

    esp_err_t ret = bme690_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bme690 init failed: %s", esp_err_to_name(ret));
        return;
    }

    printf("Sample,Temperature(C),Pressure(Pa),Humidity(%%),Gas(Ohm),Status\r\n");

    uint32_t sample = 1;
    while (1) {
        bme690_data_t data = {0};
        ret = bme690_read_forced(&data);
        if (ret == ESP_OK) {
            printf("%lu,%.2f,%.2f,%.2f,%.2f,0x%02X\r\n",
                   (unsigned long)sample,
                   data.temperature_c,
                   data.pressure_pa,
                   data.humidity_percent,
                   data.gas_resistance_ohm,
                   data.status);
            sample++;
        } else {
            ESP_LOGW(TAG, "bme690 read failed: %s", esp_err_to_name(ret));
        }

        vTaskDelay(pdMS_TO_TICKS(BME690_READ_INTERVAL_MS));
    }
}
