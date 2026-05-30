#include "bme690_llm_bridge.h"

#include <stdio.h>

#include "esp_log.h"
#include "llm_client.h"

static const char *TAG = "bme690_llm_bridge";

esp_err_t bme690_llm_bridge_init(void)
{
    ESP_LOGI(TAG, "BME690 bridge reserved");
    return ESP_OK;
}

esp_err_t bme690_llm_bridge_send_json(const char *json)
{
    if (json == NULL || json[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "BME690 JSON reserved");
    return llm_client_send_sensor_json_with_model("bme690", json, BME690_LLM_BRIDGE_LLM_MODEL);
}

esp_err_t bme690_llm_bridge_send_reading(float temperature,
                                         float humidity,
                                         float pressure,
                                         float gas_resistance)
{
    char json[192] = {0};
    int written = snprintf(json,
                           sizeof(json),
                           "{\"temperature\":%.2f,\"humidity\":%.2f,\"pressure\":%.2f,\"gas_resistance\":%.2f}",
                           temperature,
                           humidity,
                           pressure,
                           gas_resistance);
    if (written < 0 || (size_t)written >= sizeof(json)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return bme690_llm_bridge_send_json(json);
}
