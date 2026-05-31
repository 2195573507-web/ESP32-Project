#include "llm_gateway_http.h"

#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "llm_gateway_protocol.h"
#include "volc_gateway_auth.h"

static const char *TAG = "llm_gateway_http";

static esp_err_t llm_gateway_http_read_response(esp_http_client_handle_t client,
                                                char *response,
                                                size_t response_size)
{
    if (response == NULL || response_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    response[0] = '\0';

    int read_len = esp_http_client_read_response(client, response, response_size - 1);
    if (read_len < 0) {
        return ESP_FAIL;
    }
    response[read_len] = '\0';
    return ESP_OK;
}

static void llm_gateway_http_log_reject_hint(int status_code,
                                             const char *url,
                                             const char *model,
                                             const char *response)
{
    if (status_code != 401 && status_code != 403) {
        return;
    }

    char key_summary[48] = {0};
    volc_gateway_auth_make_key_summary(key_summary, sizeof(key_summary));
    ESP_LOGE(TAG,
             "Gateway Chat rejected: status=%d url=%s model=%s key=%s",
             status_code,
             url != NULL ? url : "<null>",
             model != NULL ? model : "<null>",
             key_summary);
    ESP_LOGE(TAG,
             "Check Authorization Bearer API key, check whether API key is bound to Chat model %s, "
             "check Chat model name and Chat Completions permission.",
             model != NULL ? model : "<null>");

    if (response != NULL && response[0] != '\0') {
        size_t preview_len = strlen(response);
        if (preview_len > 256U) {
            preview_len = 256U;
        }
        ESP_LOGE(TAG, "Gateway Chat error body preview: %.*s", (int)preview_len, response);
    }
}

static esp_err_t llm_gateway_http_post(const char *url,
                                       const char *content_type,
                                       const uint8_t *body,
                                       size_t body_len,
                                       const char *model,
                                       char *response,
                                       size_t response_size)
{
    if (url == NULL || content_type == NULL || body == NULL ||
        body_len == 0 || response == NULL || response_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char auth_header[256] = {0};
    esp_err_t ret = volc_gateway_auth_build_authorization(auth_header, sizeof(auth_header));
    if (ret != ESP_OK) {
        return ret;
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = LLM_GATEWAY_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_post_field(client, (const char *)body, body_len);

    ret = esp_http_client_perform(client);
    if (ret == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        if (APP_DEBUG_LLM_GATEWAY_HTTP) {
            ESP_LOGI(TAG, "HTTP POST status=%d url=%s", status_code, url);
        }
        esp_err_t read_ret = llm_gateway_http_read_response(client, response, response_size);
        if (read_ret != ESP_OK) {
            ESP_LOGW(TAG, "HTTP response read failed: %s", esp_err_to_name(read_ret));
        }
        if (status_code < 200 || status_code >= 300) {
            llm_gateway_http_log_reject_hint(status_code, url, model, response);
            ret = (status_code == 401 || status_code == 403) ? ESP_ERR_INVALID_RESPONSE : ESP_FAIL;
        } else if (read_ret != ESP_OK) {
            ret = read_ret;
        }
    }

    esp_http_client_cleanup(client);
    return ret;
}

esp_err_t llm_gateway_http_chat_completion(const char *model,
                                           const char *system_prompt,
                                           const char *user_text,
                                           char *out_text,
                                           size_t out_text_size)
{
    if (model == NULL || model[0] == '\0' ||
        user_text == NULL || user_text[0] == '\0' ||
        out_text == NULL || out_text_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out_text[0] = '\0';

    const char *url = VOLC_GATEWAY_CHAT_COMPLETIONS_URI;

    esp_err_t ret = ESP_OK;
    char *request_json = NULL;
    size_t request_len = 0;
    ret = llm_gateway_protocol_build_chat_request(model,
                                                  system_prompt,
                                                  user_text,
                                                  &request_json,
                                                  &request_len);
    if (ret != ESP_OK) {
        return ret;
    }

    char *response = (char *)malloc(LLM_GATEWAY_HTTP_RESPONSE_MAX_BYTES);
    if (response == NULL) {
        llm_gateway_protocol_free(request_json);
        return ESP_ERR_NO_MEM;
    }

    if (APP_DEBUG_LLM_GATEWAY_HTTP) {
        ESP_LOGI(TAG,
                 "LLM HTTP request start: model=%s user_len=%u",
                 model,
                 (unsigned int)strlen(user_text));
    }
    ret = llm_gateway_http_post(url,
                                "application/json",
                                (const uint8_t *)request_json,
                                request_len,
                                model,
                                response,
                                LLM_GATEWAY_HTTP_RESPONSE_MAX_BYTES);
    llm_gateway_protocol_free(request_json);
    if (ret == ESP_OK) {
        ret = llm_gateway_protocol_parse_chat_response(response, out_text, out_text_size);
    }
    free(response);
    return ret;
}
