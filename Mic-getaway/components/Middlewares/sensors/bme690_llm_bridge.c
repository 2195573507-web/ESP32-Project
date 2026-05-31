#include "bme690_llm_bridge.h"

#include <stdio.h>

#include "ai_bridge_common.h"
#include "esp_log.h"
#include "llm_client.h"

static const char *TAG = "bme690_llm_bridge";

esp_err_t bme690_llm_bridge_init(void)
{
    return ai_bme690_bridge_init();
}

esp_err_t ai_bme690_bridge_init(void)
{
    ESP_LOGI(TAG, "BME690 bridge reserved");
    return ESP_OK;
}

esp_err_t ai_bme690_bridge_build_context(float temperature_c,
                                         float humidity_rh,
                                         float pressure_hpa,
                                         float gas_resistance_ohm,
                                         float iaq,
                                         char *out_context,
                                         size_t out_context_size)
{
    esp_err_t ret = ai_bridge_check_out_buf(out_context, out_context_size, "bme690_context");
    if (ret != ESP_OK) {
        return ret;
    }

    int written = snprintf(out_context,
                           out_context_size,
                           "模块：BME690 环境传感器\n\n"
                           "当前环境数据：\n"
                           "- 温度：%.2f C\n"
                           "- 湿度：%.2f %%\n"
                           "- 气压：%.2f hPa\n"
                           "- 气体电阻：%.2f ohm\n"
                           "- IAQ：%.2f",
                           temperature_c,
                           humidity_rh,
                           pressure_hpa,
                           gas_resistance_ohm,
                           iaq);
    return (written < 0 || (size_t)written >= out_context_size) ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

esp_err_t ai_bme690_bridge_report(float temperature_c,
                                  float humidity_rh,
                                  float pressure_hpa,
                                  float gas_resistance_ohm,
                                  float iaq)
{
    char context[256] = {0};
    esp_err_t ret = ai_bme690_bridge_build_context(temperature_c,
                                                   humidity_rh,
                                                   pressure_hpa,
                                                   gas_resistance_ohm,
                                                   iaq,
                                                   context,
                                                   sizeof(context));
    if (ret != ESP_OK) {
        return ret;
    }
    ESP_LOGI(TAG, "%s", context);
    return ESP_OK;
}

esp_err_t ai_bme690_bridge_ask(float temperature_c,
                               float humidity_rh,
                               float pressure_hpa,
                               float gas_resistance_ohm,
                               float iaq,
                               const char *question,
                               char *out_reply,
                               size_t out_reply_size)
{
    esp_err_t ret = ai_bridge_check_str(question, "question");
    if (ret != ESP_OK) {
        return ret;
    }
    ret = ai_bridge_check_out_buf(out_reply, out_reply_size, "reply");
    if (ret != ESP_OK) {
        return ret;
    }

    char context[384] = {0};
    ret = ai_bme690_bridge_build_context(temperature_c,
                                         humidity_rh,
                                         pressure_hpa,
                                         gas_resistance_ohm,
                                         iaq,
                                         context,
                                         sizeof(context));
    if (ret != ESP_OK) {
        return ret;
    }

    size_t offset = strlen(context);
    ret = ai_bridge_appendf(context, sizeof(context), &offset, "\n\n用户问题：\n%s", question);
    if (ret != ESP_OK) {
        return ret;
    }
    return llm_client_text_query("你是 ESP32 设备上的环境数据分析模型。根据 BME690 数据给出简短判断，不要编造不存在的传感器数据。",
                                 context,
                                 out_reply,
                                 out_reply_size);
}

esp_err_t bme690_llm_bridge_send_json(const char *json)
{
    if (json == NULL || json[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "BME690 JSON reserved");
    return llm_client_send_bme690_json(json);
}

esp_err_t bme690_llm_bridge_send_reading(float temperature,
                                         float humidity,
                                         float pressure,
                                         float gas_resistance)
{
    return ai_bme690_bridge_report(temperature, humidity, pressure, gas_resistance, 0.0f);
}
