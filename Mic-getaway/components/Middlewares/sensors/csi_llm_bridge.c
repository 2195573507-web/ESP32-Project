#include "csi_llm_bridge.h"

#include "esp_log.h"
#include "llm_client.h"

static const char *TAG = "csi_llm_bridge";

esp_err_t csi_llm_bridge_init(void)
{
    ESP_LOGI(TAG, "CSI bridge reserved");
    return ESP_OK;
}

esp_err_t csi_llm_bridge_send_json(const char *json)
{
    if (json == NULL || json[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "CSI JSON reserved");
    return llm_client_send_sensor_json_with_model("csi", json, CSI_LLM_BRIDGE_LLM_MODEL);
}

esp_err_t csi_llm_bridge_send_summary(const char *summary_json)
{
    return csi_llm_bridge_send_json(summary_json);
}
