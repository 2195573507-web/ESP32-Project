#include "llm_client.h"
#include "http_client.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "llm_client";

static char *api_url = NULL;
static char *api_key = NULL;

esp_err_t llm_client_init(void)
{
    // 初始化HTTP客户端（如果尚未初始化）
    if (http_client_init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }

    // 设置默认API信息（请替换为你的实际信息）
    llm_set_api_url("https://ark.cn-beijing.volces.com/api/v3/chat/completions");
    llm_set_api_key("REPLACE_WITH_LLM_API_KEY");

    ESP_LOGI(TAG, "LLM client initialized");
    return ESP_OK;
}

static esp_err_t parse_llm_response(const char *json, char *output, size_t output_size, size_t *output_len)
{
    const char *assistant_role = strstr(json, "\"role\":\"assistant\"");
    const char *content_key = NULL;
    if (assistant_role != NULL) {
        content_key = strstr(assistant_role, "\"content\":");
    } else {
        content_key = strstr(json, "\"content\":");
    }
    if (content_key == NULL) {
        ESP_LOGE(TAG, "Could not find content field in LLM response");
        return ESP_FAIL;
    }

    const char *value_start = strchr(content_key, ':');
    if (value_start == NULL) {
        ESP_LOGE(TAG, "Malformed content field in LLM response");
        return ESP_FAIL;
    }
    value_start++;
    while (*value_start == ' ' || *value_start == '\t' || *value_start == '\n' || *value_start == '\r') {
        value_start++;
    }
    if (*value_start != '"') {
        ESP_LOGE(TAG, "Content value is not a JSON string");
        return ESP_FAIL;
    }
    value_start++;

    size_t out_index = 0;
    bool escape = false;
    for (const char *p = value_start; *p != '\0'; ++p) {
        if (escape) {
            switch (*p) {
                case '"': output[out_index++] = '"'; break;
                case '\\': output[out_index++] = '\\'; break;
                case '/': output[out_index++] = '/'; break;
                case 'b': output[out_index++] = '\b'; break;
                case 'f': output[out_index++] = '\f'; break;
                case 'n': output[out_index++] = '\n'; break;
                case 'r': output[out_index++] = '\r'; break;
                case 't': output[out_index++] = '\t'; break;
                default: output[out_index++] = *p; break;
            }
            escape = false;
            if (out_index >= output_size - 1) {
                break;
            }
            continue;
        }
        if (*p == '\\') {
            escape = true;
            continue;
        }
        if (*p == '"') {
            output[out_index] = '\0';
            *output_len = out_index;
            return ESP_OK;
        }
        if (out_index < output_size - 1) {
            output[out_index++] = *p;
        } else {
            break;
        }
    }

    ESP_LOGE(TAG, "LLM response content too large or unterminated");
    return ESP_FAIL;
}

esp_err_t llm_send_message(const char *message, char *response_buffer, size_t buffer_size, size_t *response_len)
{
    if (api_url == NULL || api_key == NULL) {
        ESP_LOGE(TAG, "API URL or API key not set");
        return ESP_FAIL;
    }

    size_t request_body_size = strlen(message) + 256;
    char *request_body = malloc(request_body_size);
    if (request_body == NULL) {
        ESP_LOGE(TAG, "Failed to allocate request body buffer");
        return ESP_ERR_NO_MEM;
    }

    int request_len = snprintf(request_body, request_body_size,
             "{\"model\":\"gpt-3.5-turbo\",\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}]}",
             message);
    if (request_len < 0 || (size_t)request_len >= request_body_size) {
        ESP_LOGE(TAG, "Request body overflow or formatting error");
        free(request_body);
        return ESP_ERR_INVALID_ARG;
    }

    size_t auth_header_size = strlen("Bearer ") + strlen(api_key) + 1;
    char *auth_header = malloc(auth_header_size);
    if (auth_header == NULL) {
        ESP_LOGE(TAG, "Failed to allocate auth header buffer");
        free(request_body);
        return ESP_ERR_NO_MEM;
    }
    snprintf(auth_header, auth_header_size, "Bearer %s", api_key);

    esp_err_t err = http_post_with_headers(api_url, request_body, request_len,
                                           "Authorization", auth_header,
                                           response_buffer, buffer_size, response_len);
    free(request_body);
    free(auth_header);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send request to LLM API");
        return err;
    }

    size_t parsed_len = 0;
    esp_err_t parse_err = parse_llm_response(response_buffer, response_buffer, buffer_size, &parsed_len);
    if (parse_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to parse LLM response JSON");
        return parse_err;
    }

    *response_len = parsed_len;
    ESP_LOGI(TAG, "LLM request sent successfully");
    return ESP_OK;
}

void llm_set_api_url(const char *url)
{
    if (api_url != NULL) {
        free(api_url);
    }
    api_url = strdup(url);
    ESP_LOGI(TAG, "API URL set to: %s", url);
}

void llm_set_api_key(const char *key)
{
    if (api_key != NULL) {
        free(api_key);
    }
    api_key = strdup(key);
    ESP_LOGI(TAG, "API key set");
}
