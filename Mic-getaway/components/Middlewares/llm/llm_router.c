#include "llm_router.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "llm_config.h"
#include "system_llm_bridge.h"

static const char *TAG = "llm_router";

static llm_command_type_t llm_router_command_from_string(const char *command)
{
    if (command == NULL || command[0] == '\0') {
        return LLM_COMMAND_NONE;
    }
    if (strcmp(command, "screen_on") == 0) {
        return LLM_COMMAND_SCREEN_ON;
    }
    if (strcmp(command, "screen_off") == 0) {
        return LLM_COMMAND_SCREEN_OFF;
    }
    if (strcmp(command, "screen_set_brightness") == 0) {
        return LLM_COMMAND_SCREEN_SET_BRIGHTNESS;
    }
    if (strcmp(command, "screen_show_page") == 0) {
        return LLM_COMMAND_SCREEN_SHOW_PAGE;
    }
    if (strcmp(command, "sensor_read_bme690") == 0) {
        return LLM_COMMAND_SENSOR_READ_BME690;
    }
    if (strcmp(command, "sensor_read_csi") == 0) {
        return LLM_COMMAND_SENSOR_READ_CSI;
    }
    if (strcmp(command, "audio_stop") == 0) {
        return LLM_COMMAND_AUDIO_STOP;
    }
    if (strcmp(command, "system_status") == 0) {
        return LLM_COMMAND_SYSTEM_STATUS;
    }
    return LLM_COMMAND_UNKNOWN;
}

static void llm_router_copy_string_field(const cJSON *root,
                                         const char *name,
                                         char *out,
                                         size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }
    out[0] = '\0';

    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        strlcpy(out, item->valuestring, out_size);
    }
}

esp_err_t llm_router_parse_final_text(const char *text, llm_router_result_t *out)
{
    if (text == NULL || text[0] == '\0' || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_Parse(text);
    if (root == NULL) {
        ESP_LOGW(TAG, "LLM final is not strict JSON, print only: %s", text);
        return ESP_ERR_INVALID_RESPONSE;
    }

    llm_router_copy_string_field(root, "type", out->type, sizeof(out->type));
    llm_router_copy_string_field(root, "command", out->command, sizeof(out->command));
    llm_router_copy_string_field(root, "reply", out->reply, sizeof(out->reply));

    const cJSON *speak = cJSON_GetObjectItemCaseSensitive(root, "speak");
    out->speak = cJSON_IsBool(speak) && cJSON_IsTrue(speak);
    out->command_type = llm_router_command_from_string(out->command);

    const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    if (params != NULL) {
        out->params = cJSON_Duplicate(params, true);
    }

    out->valid = strcmp(out->type, "command") == 0 || strcmp(out->type, "speech") == 0;
    if (!out->valid) {
        ESP_LOGW(TAG, "Router JSON type is invalid: %s", out->type);
    }

    cJSON_Delete(root);
    return out->valid ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t llm_router_handle_result(const llm_router_result_t *result)
{
    if (result == NULL || !result->valid) {
        return ESP_ERR_INVALID_ARG;
    }

    if (APP_DEBUG_LLM_ROUTER) {
        ESP_LOGI(TAG,
                 "router result: type=%s command=%s speak=%d reply=\"%s\"",
                 result->type,
                 result->command[0] != '\0' ? result->command : "<none>",
                 result->speak,
                 result->reply);
    }

    if (strcmp(result->type, "speech") == 0) {
        ESP_LOGI(TAG, "SPEECH reply: %s", result->reply[0] != '\0' ? result->reply : "<empty>");
        if (result->speak) {
            ESP_LOGI(TAG, "TTS disabled, speech reply is printed only");
        }
        return ESP_OK;
    }

    if (strcmp(result->type, "command") != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (LLM_ROUTER_COMMAND_SKIP_TTS && result->speak) {
        ESP_LOGI(TAG, "Command TTS skipped by config");
    }

    switch (result->command_type) {
    case LLM_COMMAND_SYSTEM_STATUS: {
        char status_json[512] = {0};
        esp_err_t ret = system_llm_bridge_get_status_json(status_json, sizeof(status_json));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "system_status failed: %s", esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGI(TAG, "COMMAND system_status: %s", status_json);
        return ESP_OK;
    }
    case LLM_COMMAND_SCREEN_ON:
    case LLM_COMMAND_SCREEN_OFF:
    case LLM_COMMAND_SCREEN_SET_BRIGHTNESS:
    case LLM_COMMAND_SCREEN_SHOW_PAGE:
    case LLM_COMMAND_SENSOR_READ_BME690:
    case LLM_COMMAND_SENSOR_READ_CSI:
    case LLM_COMMAND_AUDIO_STOP:
        ESP_LOGI(TAG, "COMMAND TODO: %s", result->command);
        return ESP_OK;
    case LLM_COMMAND_NONE:
        ESP_LOGW(TAG, "COMMAND missing command name");
        return ESP_ERR_INVALID_ARG;
    case LLM_COMMAND_UNKNOWN:
    default:
        ESP_LOGW(TAG, "COMMAND unknown: %s", result->command);
        return ESP_ERR_NOT_SUPPORTED;
    }
}

void llm_router_result_cleanup(llm_router_result_t *result)
{
    if (result == NULL) {
        return;
    }
    if (result->params != NULL) {
        cJSON_Delete(result->params);
        result->params = NULL;
    }
}
