#include "llm_gateway_http.h"

#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "llm_gateway_protocol.h"
#include "volc_gateway_auth.h"

static const char *TAG = "llm_gateway_http";

#define LLM_GATEWAY_HTTP_RESPONSE_MIN_BYTES     8192U
#define LLM_GATEWAY_HTTP_RESPONSE_PREVIEW_BYTES 1024U
#define LLM_GATEWAY_HTTP_READ_CHUNK_BYTES       512
#define LLM_GATEWAY_HTTP_MAX_EMPTY_READS        3

#if LLM_GATEWAY_HTTP_RESPONSE_MAX_BYTES < LLM_GATEWAY_HTTP_RESPONSE_MIN_BYTES
#define LLM_GATEWAY_HTTP_CHAT_RESPONSE_BODY_BYTES LLM_GATEWAY_HTTP_RESPONSE_MIN_BYTES
#else
#define LLM_GATEWAY_HTTP_CHAT_RESPONSE_BODY_BYTES LLM_GATEWAY_HTTP_RESPONSE_MAX_BYTES
#endif
#define LLM_GATEWAY_HTTP_CHAT_RESPONSE_BUFFER_BYTES (LLM_GATEWAY_HTTP_CHAT_RESPONSE_BODY_BYTES + 1U)

static esp_err_t llm_gateway_http_read_response(esp_http_client_handle_t client,
                                                char *response,
                                                size_t response_size,
                                                size_t *out_response_len)
{
    if (client == NULL || response == NULL || response_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    response[0] = '\0';
    if (out_response_len != NULL) {
        *out_response_len = 0;
    }

    size_t total_len = 0;
    int empty_reads = 0;
    while (total_len < response_size - 1U) {
        size_t remaining = response_size - 1U - total_len;
        int read_size = remaining > LLM_GATEWAY_HTTP_READ_CHUNK_BYTES ?
            LLM_GATEWAY_HTTP_READ_CHUNK_BYTES : (int)remaining;
        int read_len = esp_http_client_read(client, response + total_len, read_size);
        if (read_len > 0) {
            total_len += (size_t)read_len;
            response[total_len] = '\0';
            empty_reads = 0;
            continue;
        }
        if (read_len < 0) {
            if (out_response_len != NULL) {
                *out_response_len = total_len;
            }
            ESP_LOGE(TAG, "Chat HTTP response read failed: read_len=%d body_len=%u", read_len, (unsigned int)total_len);
            return ESP_FAIL;
        }

        if (esp_http_client_is_complete_data_received(client)) {
            break;
        }

        int64_t content_length = esp_http_client_get_content_length(client);
        if (content_length >= 0 && total_len >= (size_t)content_length) {
            break;
        }

        empty_reads++;
        if (empty_reads >= LLM_GATEWAY_HTTP_MAX_EMPTY_READS) {
            if (out_response_len != NULL) {
                *out_response_len = total_len;
            }
            ESP_LOGE(TAG,
                     "Chat HTTP response read incomplete: body_len=%u content_length=%lld complete=%d",
                     (unsigned int)total_len,
                     (long long)content_length,
                     esp_http_client_is_complete_data_received(client) ? 1 : 0);
            return ESP_ERR_TIMEOUT;
        }
    }

    response[total_len] = '\0';
    if (out_response_len != NULL) {
        *out_response_len = total_len;
    }

    if (!esp_http_client_is_complete_data_received(client)) {
        int64_t content_length = esp_http_client_get_content_length(client);
        if (content_length < 0 || total_len < (size_t)content_length) {
            ESP_LOGE(TAG,
                     "Chat HTTP response body too large or incomplete: body_len=%u capacity=%u content_length=%lld",
                     (unsigned int)total_len,
                     (unsigned int)response_size,
                     (long long)content_length);
            return ESP_ERR_INVALID_SIZE;
        }
    }

    return ESP_OK;
}

static void llm_gateway_http_log_chat_200_response(const char *response, size_t response_len)
{
    if (!APP_DEBUG_LLM_GATEWAY_HTTP) {
        return;
    }

    if (response == NULL || response_len == 0 || response[0] == '\0') {
        ESP_LOGI(TAG, "Chat HTTP 200 but body empty");
        return;
    }

    size_t preview_len = response_len;
    if (preview_len > LLM_GATEWAY_HTTP_RESPONSE_PREVIEW_BYTES) {
        preview_len = LLM_GATEWAY_HTTP_RESPONSE_PREVIEW_BYTES;
    }
    ESP_LOGI(TAG, "Chat HTTP response body preview: %.*s", (int)preview_len, response);
}

static void llm_gateway_http_log_error_response(int status_code,
                                                const char *url,
                                                const char *model,
                                                const char *response)
{
    char key_summary[48] = {0};
    volc_gateway_auth_make_key_summary(key_summary, sizeof(key_summary));
    const char *chat_model = (model != NULL && model[0] != '\0') ? model : LLM_CHAT_MODEL;
    if (status_code == 403) {
        ESP_LOGE(TAG,
                 "Gateway Chat permission/model binding error: status=403 url=%s chat_model=%s key=%s",
                 url != NULL ? url : "<null>",
                 chat_model,
                 key_summary);
        ESP_LOGE(TAG,
                 "Chat HTTP 403: check gateway Chat permission and model binding for %s",
                 chat_model);
    } else {
        ESP_LOGE(TAG,
                 "Gateway Chat HTTP error: status=%d url=%s chat_model=%s key=%s",
                 status_code,
                 url != NULL ? url : "<null>",
                 chat_model,
                 key_summary);
    }

    if (response != NULL && response[0] != '\0') {
        size_t preview_len = strlen(response);
        if (preview_len > 512U) {
            preview_len = 512U;
        }
        ESP_LOGE(TAG, "Gateway Chat error body preview: %.*s", (int)preview_len, response);
    } else {
        ESP_LOGE(TAG, "Chat HTTP error body empty");
    }
}

static esp_err_t llm_gateway_http_post(const char *url,
                                       const char *content_type,
                                       const uint8_t *body,
                                       size_t body_len,
                                       const char *model,
                                       char *response,
                                       size_t response_size,
                                       int *out_status_code)
{
    if (url == NULL || content_type == NULL || body == NULL ||
        body_len == 0 || response == NULL || response_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_status_code != NULL) {
        *out_status_code = 0;
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

    ret = esp_http_client_open(client, (int)body_len);
    if (ret == ESP_OK) {
        size_t written_len = 0;
        while (written_len < body_len) {
            int write_len = esp_http_client_write(client,
                                                  (const char *)body + written_len,
                                                  (int)(body_len - written_len));
            if (write_len < 0) {
                ESP_LOGE(TAG, "Chat HTTP request body write failed: write_len=%d", write_len);
                ret = ESP_FAIL;
                break;
            }
            if (write_len == 0) {
                ESP_LOGE(TAG,
                         "Chat HTTP request body write stalled: written=%u total=%u",
                         (unsigned int)written_len,
                         (unsigned int)body_len);
                ret = ESP_ERR_TIMEOUT;
                break;
            }
            written_len += (size_t)write_len;
        }
    }

    if (ret == ESP_OK) {
        int64_t header_ret = esp_http_client_fetch_headers(client);
        if (header_ret < 0) {
            ESP_LOGE(TAG, "Chat HTTP fetch headers failed: %lld", (long long)header_ret);
            ret = ESP_FAIL;
        }
    }

    if (ret == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        if (out_status_code != NULL) {
            *out_status_code = status_code;
        }
        if (APP_DEBUG_LLM_GATEWAY_HTTP) {
            ESP_LOGI(TAG, "HTTP POST status=%d url=%s", status_code, url);
        }
        size_t response_len = 0;
        esp_err_t read_ret = llm_gateway_http_read_response(client, response, response_size, &response_len);
        if (status_code == 200) {
            llm_gateway_http_log_chat_200_response(response, response_len);
        }
        if (read_ret != ESP_OK) {
            ESP_LOGW(TAG, "HTTP response read failed: %s", esp_err_to_name(read_ret));
        }
        if (status_code < 200 || status_code >= 300) {
            llm_gateway_http_log_error_response(status_code, url, model, response);
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
                                           size_t out_text_size,
                                           int *out_status_code)
{
    if (model == NULL || model[0] == '\0' ||
        user_text == NULL || user_text[0] == '\0' ||
        out_text == NULL || out_text_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out_text[0] = '\0';
    if (out_status_code != NULL) {
        *out_status_code = 0;
    }

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

    char *response = (char *)malloc(LLM_GATEWAY_HTTP_CHAT_RESPONSE_BUFFER_BYTES);
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
                                LLM_GATEWAY_HTTP_CHAT_RESPONSE_BUFFER_BYTES,
                                out_status_code);
    llm_gateway_protocol_free(request_json);
    if (ret == ESP_OK) {
        ret = llm_gateway_protocol_parse_chat_response(response, out_text, out_text_size);
    }
    free(response);
    return ret;
}
