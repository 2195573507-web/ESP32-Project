#include "llm_gateway_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "llm_gateway_protocol.h"
#include "llm_wav.h"

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

static esp_err_t llm_gateway_http_post(const char *url,
                                       const char *content_type,
                                       const uint8_t *body,
                                       size_t body_len,
                                       char *response,
                                       size_t response_size)
{
    if (url == NULL || content_type == NULL || body == NULL ||
        body_len == 0 || response == NULL || response_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char auth_header[256] = {0};
    esp_err_t ret = llm_gateway_protocol_build_auth_header(auth_header, sizeof(auth_header));
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
        if (status_code < 200 || status_code >= 300) {
            ret = ESP_FAIL;
        } else {
            ret = llm_gateway_http_read_response(client, response, response_size);
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

    char url[256] = {0};
    esp_err_t ret = llm_gateway_protocol_build_url(LLM_GATEWAY_HTTP_BASE_URL,
                                                   LLM_GATEWAY_LLM_HTTP_PATH,
                                                   url,
                                                   sizeof(url));
    if (ret != ESP_OK) {
        return ret;
    }

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
        ESP_LOGI(TAG, "LLM HTTP request start: user_len=%u", (unsigned int)strlen(user_text));
    }
    ret = llm_gateway_http_post(url,
                                "application/json",
                                (const uint8_t *)request_json,
                                request_len,
                                response,
                                LLM_GATEWAY_HTTP_RESPONSE_MAX_BYTES);
    llm_gateway_protocol_free(request_json);
    if (ret == ESP_OK) {
        ret = llm_gateway_protocol_parse_chat_response(response, out_text, out_text_size);
    }
    free(response);
    return ret;
}

static esp_err_t llm_gateway_http_build_multipart(const uint8_t *audio,
                                                  size_t audio_bytes,
                                                  const char *model,
                                                  bool is_wav,
                                                  uint8_t **out_body,
                                                  size_t *out_body_len,
                                                  char *content_type,
                                                  size_t content_type_size)
{
    if (audio == NULL || audio_bytes == 0 ||
        model == NULL || model[0] == '\0' ||
        out_body == NULL || out_body_len == NULL ||
        content_type == NULL || content_type_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_body = NULL;
    *out_body_len = 0;

    const char *boundary = "----esp32-llm-gateway-boundary";
    const char *filename = is_wav ? "audio.wav" : "audio.pcm";
    const char *mime = is_wav ? "audio/wav" : "application/octet-stream";

    char prefix[512] = {0};
    int prefix_len = snprintf(prefix,
                              sizeof(prefix),
                              "--%s\r\n"
                              "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
                              "%s\r\n"
                              "--%s\r\n"
                              "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
                              "Content-Type: %s\r\n\r\n",
                              boundary,
                              model,
                              boundary,
                              filename,
                              mime);
    char suffix[96] = {0};
    int suffix_len = snprintf(suffix, sizeof(suffix), "\r\n--%s--\r\n", boundary);
    if (prefix_len < 0 || suffix_len < 0 ||
        (size_t)prefix_len >= sizeof(prefix) ||
        (size_t)suffix_len >= sizeof(suffix)) {
        return ESP_ERR_INVALID_SIZE;
    }

    int ct_len = snprintf(content_type,
                          content_type_size,
                          "multipart/form-data; boundary=%s",
                          boundary);
    if (ct_len < 0 || (size_t)ct_len >= content_type_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t body_len = (size_t)prefix_len + audio_bytes + (size_t)suffix_len;
    uint8_t *body = (uint8_t *)malloc(body_len);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memcpy(body, prefix, (size_t)prefix_len);
    memcpy(body + prefix_len, audio, audio_bytes);
    memcpy(body + prefix_len + audio_bytes, suffix, (size_t)suffix_len);

    *out_body = body;
    *out_body_len = body_len;
    return ESP_OK;
}

esp_err_t llm_gateway_http_asr_transcription(const char *model,
                                             const int16_t *pcm,
                                             size_t samples,
                                             uint32_t sample_rate_hz,
                                             char *out_text,
                                             size_t out_text_size)
{
    if (model == NULL || model[0] == '\0' ||
        pcm == NULL || samples == 0 || out_text == NULL || out_text_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out_text[0] = '\0';

    char url[256] = {0};
    esp_err_t ret = llm_gateway_protocol_build_url(LLM_GATEWAY_HTTP_BASE_URL,
                                                   LLM_GATEWAY_ASR_HTTP_PATH,
                                                   url,
                                                   sizeof(url));
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t *upload_audio = NULL;
    size_t upload_audio_bytes = 0;
    bool upload_as_wav = LLM_GATEWAY_ASR_UPLOAD_AS_WAV != 0;
    if (upload_as_wav) {
        ret = llm_wav_build_from_pcm16(pcm,
                                       samples,
                                       sample_rate_hz,
                                       LLM_GATEWAY_AUDIO_CHANNELS,
                                       &upload_audio,
                                       &upload_audio_bytes);
        if (ret != ESP_OK) {
            return ret;
        }
    } else {
        upload_audio = (uint8_t *)pcm;
        upload_audio_bytes = samples * sizeof(int16_t);
    }

    uint8_t *body = NULL;
    size_t body_len = 0;
    char content_type[96] = {0};
    ret = llm_gateway_http_build_multipart(upload_audio,
                                           upload_audio_bytes,
                                           model,
                                           upload_as_wav,
                                           &body,
                                           &body_len,
                                           content_type,
                                           sizeof(content_type));
    if (upload_as_wav) {
        free(upload_audio);
    }
    if (ret != ESP_OK) {
        return ret;
    }

    char *response = (char *)malloc(LLM_GATEWAY_HTTP_RESPONSE_MAX_BYTES);
    if (response == NULL) {
        free(body);
        return ESP_ERR_NO_MEM;
    }

    if (APP_DEBUG_LLM_GATEWAY_HTTP) {
        ESP_LOGI(TAG,
                 "ASR HTTP fallback start: samples=%u, body_bytes=%u",
                 (unsigned int)samples,
                 (unsigned int)body_len);
    }
    ret = llm_gateway_http_post(url,
                                content_type,
                                body,
                                body_len,
                                response,
                                LLM_GATEWAY_HTTP_RESPONSE_MAX_BYTES);
    free(body);
    if (ret == ESP_OK) {
        ret = llm_gateway_protocol_parse_asr_http_response(response, out_text, out_text_size);
    }
    free(response);
    return ret;
}
