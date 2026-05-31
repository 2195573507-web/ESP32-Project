#include "volc_gateway_auth.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "llm_config.h"

esp_err_t volc_gateway_auth_build_authorization(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(out,
                           out_size,
                           "%s%s",
                           LLM_GATEWAY_AUTH_BEARER_PREFIX,
                           VOLC_GATEWAY_API_KEY);
    if (written < 0 || (size_t)written >= out_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t volc_gateway_auth_build_ws_headers(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char auth_value[256] = {0};
    esp_err_t ret = volc_gateway_auth_build_authorization(auth_value, sizeof(auth_value));
    if (ret != ESP_OK) {
        return ret;
    }

    int written = snprintf(out,
                           out_size,
                           "Authorization: %s\r\n"
#if VOLC_GATEWAY_USE_RESOURCE_ID
                           "X-Api-Resource-Id: %s\r\n"
#endif
                           ,
                           auth_value
#if VOLC_GATEWAY_USE_RESOURCE_ID
                           , VOLC_GATEWAY_ASR_RESOURCE_ID
#endif
                           );
    if (written < 0 || (size_t)written >= out_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t volc_gateway_auth_build_tts_ws_headers(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char auth_value[256] = {0};
    esp_err_t ret = volc_gateway_auth_build_authorization(auth_value, sizeof(auth_value));
    if (ret != ESP_OK) {
        return ret;
    }

    int written = snprintf(out,
                           out_size,
                           "Authorization: %s\r\n"
#if VOLC_GATEWAY_TTS_USE_RESOURCE_ID
                           "X-Api-Resource-Id: %s\r\n"
#endif
                           ,
                           auth_value
#if VOLC_GATEWAY_TTS_USE_RESOURCE_ID
                           , VOLC_GATEWAY_TTS_RESOURCE_ID
#endif
                           );
    if (written < 0 || (size_t)written >= out_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

void volc_gateway_auth_make_key_summary(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }

    size_t key_len = strlen(VOLC_GATEWAY_API_KEY);
    if (key_len < 7) {
        snprintf(out, out_size, "len=%u, masked=***", (unsigned int)key_len);
        return;
    }

    snprintf(out,
             out_size,
             "len=%u, masked=%.3s***%s",
             (unsigned int)key_len,
             VOLC_GATEWAY_API_KEY,
             VOLC_GATEWAY_API_KEY + key_len - 3);
}

bool volc_gateway_auth_has_placeholder(void)
{
    return strstr(VOLC_GATEWAY_API_KEY, LLM_GATEWAY_PLACEHOLDER_MARKER) != NULL;
}
