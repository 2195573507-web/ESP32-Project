#include "llm_client.h"
#include "http_client.h"
#include "esp_log.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

static const char *TAG = "llm_client";

static char *api_url = NULL;
static char *api_key = NULL;

/**
 * @brief 检查 OpenAI-compatible Base URL 是否符合拼接要求。
 *
 * 调用方法：llm_client_init() 写入 OPENAI_CHAT_COMPLETIONS_URL 前调用。当前配置保持
 * OpenAI 库习惯：OPENAI_BASE_URL 单独保存，最终请求 URL 由
 * OPENAI_BASE_URL + "chat/completions" 拼出，因此 Base URL 必须以 '/' 结尾。
 */
static esp_err_t llm_client_validate_base_url(void)
{
    size_t base_url_len = strlen(OPENAI_BASE_URL);
    if (base_url_len == 0 || OPENAI_BASE_URL[base_url_len - 1] != '/') {
        ESP_LOGE(TAG,
                 "OPENAI_BASE_URL must end with '/': current=\"%s\"",
                 OPENAI_BASE_URL);
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

/**
 * @brief 计算 JSON 字符串转义后的长度。
 *
 * 调用方法：llm_client_json_escape_alloc() 先调用本函数计算目标缓冲大小，再逐字符写入。
 * 这里至少处理双引号、反斜杠、换行、回车、tab；其它 ASCII 控制字符写成 \u00XX，
 * 避免 ASR 文本中的特殊字符破坏 chat/completions JSON。
 */
static esp_err_t llm_client_json_escaped_length(const char *input, size_t *escaped_len)
{
    if (input == NULL || escaped_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t total_len = 0;
    for (const unsigned char *p = (const unsigned char *)input; *p != '\0'; ++p) {
        size_t add_len = 1;
        switch (*p) {
        case '"':
        case '\\':
        case '\n':
        case '\r':
        case '\t':
        case '\b':
        case '\f':
            add_len = 2;
            break;
        default:
            if (*p < 0x20) {
                add_len = 6;
            }
            break;
        }

        if (total_len > SIZE_MAX - add_len) {
            return ESP_ERR_INVALID_SIZE;
        }
        total_len += add_len;
    }

    *escaped_len = total_len;
    return ESP_OK;
}

/**
 * @brief 复制并转义一段 JSON 字符串值。
 *
 * 调用方法：llm_send_message() 组装请求体前，对 system prompt、user prompt 和 user id
 * 逐项转义。输出不包含两侧双引号，由调用方放入 JSON 模板。
 */
static esp_err_t llm_client_json_escape_alloc(const char *input, char **escaped_output)
{
    if (input == NULL || escaped_output == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *escaped_output = NULL;

    size_t escaped_len = 0;
    esp_err_t len_ret = llm_client_json_escaped_length(input, &escaped_len);
    if (len_ret != ESP_OK) {
        return len_ret;
    }
    if (escaped_len == SIZE_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    char *output = malloc(escaped_len + 1);
    if (output == NULL) {
        ESP_LOGE(TAG, "Failed to allocate JSON escape buffer");
        return ESP_ERR_NO_MEM;
    }

    char *cursor = output;
    for (const unsigned char *p = (const unsigned char *)input; *p != '\0'; ++p) {
        switch (*p) {
        case '"':
            *cursor++ = '\\';
            *cursor++ = '"';
            break;
        case '\\':
            *cursor++ = '\\';
            *cursor++ = '\\';
            break;
        case '\n':
            *cursor++ = '\\';
            *cursor++ = 'n';
            break;
        case '\r':
            *cursor++ = '\\';
            *cursor++ = 'r';
            break;
        case '\t':
            *cursor++ = '\\';
            *cursor++ = 't';
            break;
        case '\b':
            *cursor++ = '\\';
            *cursor++ = 'b';
            break;
        case '\f':
            *cursor++ = '\\';
            *cursor++ = 'f';
            break;
        default:
            if (*p < 0x20) {
                snprintf(cursor, 7, "\\u%04X", (unsigned int)*p);
                cursor += 6;
            } else {
                *cursor++ = (char)*p;
            }
            break;
        }
    }
    *cursor = '\0';

    *escaped_output = output;
    return ESP_OK;
}

esp_err_t llm_client_init(void)
{
    esp_err_t base_url_ret = llm_client_validate_base_url();
    if (base_url_ret != ESP_OK) {
        return base_url_ret;
    }

    /* 通用客户端只依赖底层 HTTP 模块，不关心 WiFi 初始化和业务主流程。 */
    if (http_client_init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }

    /* 统一写入 llm_config.h 的 OpenAI-compatible LLM 配置。 */
    llm_set_api_url(OPENAI_CHAT_COMPLETIONS_URL);
    llm_set_api_key(OPENAI_API_KEY);

    ESP_LOGI(TAG, "LLM client initialized");
    return ESP_OK;
}

/**
 * @brief 从 OpenAI-compatible JSON 响应中提取 assistant.content。
 *
 * 调用方法：llm_send_message() 收到完整 HTTP 响应后调用。这里延续原始通用客户端的
 * 轻量字符串扫描方式，不引入 cJSON 依赖；Mic 侧如需更严格解析，可在适配层另行处理。
 */
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
    if (message == NULL || message[0] == '\0' ||
        response_buffer == NULL || buffer_size <= 1 || response_len == NULL) {
        ESP_LOGE(TAG, "Invalid LLM send_message arguments");
        return ESP_ERR_INVALID_ARG;
    }

    if (api_url == NULL || api_key == NULL) {
        ESP_LOGE(TAG, "API URL or API key not set");
        return ESP_FAIL;
    }

    char *escaped_model = NULL;
    char *escaped_system_prompt = NULL;
    char *escaped_message = NULL;
    char *escaped_user_id = NULL;

    esp_err_t escape_ret = llm_client_json_escape_alloc(OPENAI_MODEL_NAME, &escaped_model);
    if (escape_ret == ESP_OK) {
        escape_ret = llm_client_json_escape_alloc(OPENAI_SYSTEM_PROMPT, &escaped_system_prompt);
    }
    if (escape_ret == ESP_OK) {
        escape_ret = llm_client_json_escape_alloc(message, &escaped_message);
    }
    if (escape_ret == ESP_OK) {
        escape_ret = llm_client_json_escape_alloc(OPENAI_USER_ID, &escaped_user_id);
    }
    if (escape_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to escape LLM request JSON: %s", esp_err_to_name(escape_ret));
        free(escaped_model);
        free(escaped_system_prompt);
        free(escaped_message);
        free(escaped_user_id);
        return escape_ret;
    }

    size_t request_body_size = strlen(escaped_message) +
                               strlen(escaped_model) +
                               strlen(escaped_system_prompt) +
                               strlen(escaped_user_id) +
                               LLM_CLIENT_REQUEST_EXTRA_BYTES;
    char *request_body = malloc(request_body_size);
    if (request_body == NULL) {
        ESP_LOGE(TAG, "Failed to allocate request body buffer");
        free(escaped_model);
        free(escaped_system_prompt);
        free(escaped_message);
        free(escaped_user_id);
        return ESP_ERR_NO_MEM;
    }

    int request_len = snprintf(request_body, request_body_size,
             "{\"model\":\"%s\",\"messages\":[{\"role\":\"system\",\"content\":\"%s\"},"
             "{\"role\":\"user\",\"content\":\"%s\"}],\"user\":\"%s\"}",
             escaped_model,
             escaped_system_prompt,
             escaped_message,
             escaped_user_id);
    free(escaped_model);
    free(escaped_system_prompt);
    free(escaped_message);
    free(escaped_user_id);
    if (request_len < 0 || (size_t)request_len >= request_body_size) {
        ESP_LOGE(TAG, "Request body overflow or formatting error");
        free(request_body);
        return ESP_ERR_INVALID_ARG;
    }

    size_t auth_header_size = strlen(LLM_CLIENT_AUTH_BEARER_PREFIX) + strlen(api_key) + 1;
    char *auth_header = malloc(auth_header_size);
    if (auth_header == NULL) {
        ESP_LOGE(TAG, "Failed to allocate auth header buffer");
        free(request_body);
        return ESP_ERR_NO_MEM;
    }
    snprintf(auth_header, auth_header_size, "%s%s", LLM_CLIENT_AUTH_BEARER_PREFIX, api_key);

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
    ESP_LOGI(TAG, "LLM JSON parse success, content_len=%u", (unsigned int)parsed_len);
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
