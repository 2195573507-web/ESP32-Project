#include "llm_gateway_ws.h"

#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "llm_config.h"
#include "llm_gateway_protocol.h"

static const char *TAG = "llm_gateway_ws";

enum {
    LLM_GATEWAY_WS_CONNECTED_BIT = BIT0,
    LLM_GATEWAY_WS_ERROR_BIT = BIT1,
};

typedef struct {
    esp_websocket_client_handle_t client;
    EventGroupHandle_t event_group;
    const char *asr_model;
    llm_gateway_ws_event_cb_t event_cb;
    void *user_ctx;
    char *rx_buffer;
    size_t rx_buffer_len;
    size_t rx_buffer_expected;
    bool connected;
} llm_gateway_ws_state_t;

static llm_gateway_ws_state_t s_ws;

static void llm_gateway_ws_emit(llm_gateway_ws_event_type_t type,
                                const char *text,
                                size_t audio_len,
                                int code,
                                const char *message)
{
    if (s_ws.event_cb == NULL) {
        return;
    }

    llm_gateway_ws_event_t event = {
        .type = type,
        .text = text,
        .audio_len = audio_len,
        .code = code,
        .message = message,
    };
    s_ws.event_cb(&event, s_ws.user_ctx);
}

static void llm_gateway_ws_clear_rx_buffer(void)
{
    free(s_ws.rx_buffer);
    s_ws.rx_buffer = NULL;
    s_ws.rx_buffer_len = 0;
    s_ws.rx_buffer_expected = 0;
}

static void llm_gateway_ws_handle_payload(const char *payload, size_t payload_len)
{
    llm_gateway_asr_event_t parsed = {0};
    esp_err_t ret = llm_gateway_protocol_parse_asr_ws_event(payload, payload_len, &parsed);
    if (ret != ESP_OK) {
        if (APP_DEBUG_LLM_GATEWAY_PROTO) {
            ESP_LOGD(TAG, "Skip non-JSON or unrecognized WS payload: len=%u", (unsigned int)payload_len);
        }
        return;
    }

    if (parsed.has_audio) {
        llm_gateway_ws_emit(LLM_GATEWAY_WS_EVENT_TTS_AUDIO,
                            NULL,
                            parsed.audio_len,
                            parsed.code,
                            parsed.message);
    }
    if (parsed.is_error) {
        llm_gateway_ws_emit(LLM_GATEWAY_WS_EVENT_ERROR,
                            parsed.text,
                            0,
                            parsed.code,
                            parsed.message[0] != '\0' ? parsed.message : "ASR WebSocket error");
        return;
    }
    if (parsed.is_final && parsed.text[0] != '\0') {
        llm_gateway_ws_emit(LLM_GATEWAY_WS_EVENT_ASR_FINAL,
                            parsed.text,
                            0,
                            parsed.code,
                            parsed.message);
        return;
    }
    if (parsed.is_partial && parsed.text[0] != '\0') {
        llm_gateway_ws_emit(LLM_GATEWAY_WS_EVENT_ASR_PARTIAL,
                            parsed.text,
                            0,
                            parsed.code,
                            parsed.message);
    }
}

static void llm_gateway_ws_handle_data(const esp_websocket_event_data_t *data)
{
    if (data == NULL || data->data_ptr == NULL || data->data_len <= 0) {
        return;
    }

    if (data->payload_len <= data->data_len && data->payload_offset == 0) {
        char *payload = (char *)malloc((size_t)data->data_len + 1U);
        if (payload == NULL) {
            llm_gateway_ws_emit(LLM_GATEWAY_WS_EVENT_ERROR, NULL, 0, ESP_ERR_NO_MEM, "WS RX alloc failed");
            return;
        }
        memcpy(payload, data->data_ptr, (size_t)data->data_len);
        payload[data->data_len] = '\0';
        llm_gateway_ws_handle_payload(payload, (size_t)data->data_len);
        free(payload);
        return;
    }

    if (data->payload_offset == 0) {
        llm_gateway_ws_clear_rx_buffer();
        if (data->payload_len <= 0 || data->payload_len > LLM_GATEWAY_HTTP_RESPONSE_MAX_BYTES) {
            llm_gateway_ws_emit(LLM_GATEWAY_WS_EVENT_ERROR, NULL, 0, ESP_ERR_INVALID_SIZE, "WS payload too large");
            return;
        }
        s_ws.rx_buffer = (char *)malloc((size_t)data->payload_len + 1U);
        if (s_ws.rx_buffer == NULL) {
            llm_gateway_ws_emit(LLM_GATEWAY_WS_EVENT_ERROR, NULL, 0, ESP_ERR_NO_MEM, "WS fragmented RX alloc failed");
            return;
        }
        s_ws.rx_buffer_expected = (size_t)data->payload_len;
        s_ws.rx_buffer_len = 0;
    }

    if (s_ws.rx_buffer == NULL ||
        (size_t)data->payload_offset != s_ws.rx_buffer_len ||
        s_ws.rx_buffer_len + (size_t)data->data_len > s_ws.rx_buffer_expected) {
        llm_gateway_ws_clear_rx_buffer();
        llm_gateway_ws_emit(LLM_GATEWAY_WS_EVENT_ERROR, NULL, 0, ESP_ERR_INVALID_STATE, "WS fragmented RX order invalid");
        return;
    }

    memcpy(&s_ws.rx_buffer[s_ws.rx_buffer_len], data->data_ptr, (size_t)data->data_len);
    s_ws.rx_buffer_len += (size_t)data->data_len;
    if (s_ws.rx_buffer_len == s_ws.rx_buffer_expected) {
        s_ws.rx_buffer[s_ws.rx_buffer_len] = '\0';
        llm_gateway_ws_handle_payload(s_ws.rx_buffer, s_ws.rx_buffer_len);
        llm_gateway_ws_clear_rx_buffer();
    }
}

static void llm_gateway_ws_event_handler(void *handler_args,
                                         esp_event_base_t base,
                                         int32_t event_id,
                                         void *event_data)
{
    (void)handler_args;
    (void)base;

    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        s_ws.connected = true;
        xEventGroupSetBits(s_ws.event_group, LLM_GATEWAY_WS_CONNECTED_BIT);
        llm_gateway_ws_emit(LLM_GATEWAY_WS_EVENT_CONNECTED, NULL, 0, 0, NULL);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        s_ws.connected = false;
        llm_gateway_ws_emit(LLM_GATEWAY_WS_EVENT_DISCONNECTED, NULL, 0, 0, NULL);
        break;
    case WEBSOCKET_EVENT_DATA:
        llm_gateway_ws_handle_data(data);
        break;
    case WEBSOCKET_EVENT_ERROR:
        s_ws.connected = false;
        xEventGroupSetBits(s_ws.event_group, LLM_GATEWAY_WS_ERROR_BIT);
        llm_gateway_ws_emit(LLM_GATEWAY_WS_EVENT_ERROR,
                            NULL,
                            0,
                            data != NULL ? data->error_handle.esp_tls_last_esp_err : ESP_FAIL,
                            "WebSocket transport error");
        break;
    default:
        break;
    }
}

esp_err_t llm_gateway_ws_start(const llm_gateway_ws_config_t *config)
{
    if (config == NULL || config->asr_model == NULL || config->asr_model[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ws.client != NULL) {
        (void)llm_gateway_ws_stop();
    }
    memset(&s_ws, 0, sizeof(s_ws));
    s_ws.asr_model = config->asr_model;
    s_ws.event_cb = config->event_cb;
    s_ws.user_ctx = config->user_ctx;
    s_ws.event_group = xEventGroupCreate();
    if (s_ws.event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char url[256] = {0};
    esp_err_t ret = llm_gateway_protocol_build_url(LLM_GATEWAY_WS_BASE_URL,
                                                   LLM_GATEWAY_ASR_WS_PATH,
                                                   url,
                                                   sizeof(url));
    if (ret != ESP_OK) {
        vEventGroupDelete(s_ws.event_group);
        memset(&s_ws, 0, sizeof(s_ws));
        return ret;
    }

    char auth_header[256] = {0};
    ret = llm_gateway_protocol_build_auth_header(auth_header, sizeof(auth_header));
    if (ret != ESP_OK) {
        vEventGroupDelete(s_ws.event_group);
        memset(&s_ws, 0, sizeof(s_ws));
        return ret;
    }

    char headers[320] = {0};
    int header_len = snprintf(headers, sizeof(headers), "Authorization: %s\r\n", auth_header);
    if (header_len < 0 || (size_t)header_len >= sizeof(headers)) {
        vEventGroupDelete(s_ws.event_group);
        memset(&s_ws, 0, sizeof(s_ws));
        return ESP_ERR_INVALID_SIZE;
    }

    esp_websocket_client_config_t ws_config = {
        .uri = url,
        .headers = headers,
        .disable_auto_reconnect = true,
        .task_name = "llm_asr_ws",
        .task_stack = 8192,
        .task_prio = 4,
        .buffer_size = 2048,
        .network_timeout_ms = LLM_GATEWAY_WS_CONNECT_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    s_ws.client = esp_websocket_client_init(&ws_config);
    if (s_ws.client == NULL) {
        vEventGroupDelete(s_ws.event_group);
        memset(&s_ws, 0, sizeof(s_ws));
        return ESP_ERR_NO_MEM;
    }

    ret = esp_websocket_register_events(s_ws.client,
                                        WEBSOCKET_EVENT_ANY,
                                        llm_gateway_ws_event_handler,
                                        NULL);
    if (ret != ESP_OK) {
        esp_websocket_client_destroy(s_ws.client);
        vEventGroupDelete(s_ws.event_group);
        memset(&s_ws, 0, sizeof(s_ws));
        return ret;
    }

    ret = esp_websocket_client_start(s_ws.client);
    if (ret != ESP_OK) {
        (void)llm_gateway_ws_stop();
        return ret;
    }

    EventBits_t bits = xEventGroupWaitBits(s_ws.event_group,
                                           LLM_GATEWAY_WS_CONNECTED_BIT | LLM_GATEWAY_WS_ERROR_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(LLM_GATEWAY_WS_CONNECT_TIMEOUT_MS));
    if ((bits & LLM_GATEWAY_WS_CONNECTED_BIT) == 0) {
        (void)llm_gateway_ws_stop();
        return ESP_ERR_TIMEOUT;
    }

    char *start_json = NULL;
    size_t start_len = 0;
    ret = llm_gateway_protocol_build_asr_ws_start_event(s_ws.asr_model, &start_json, &start_len);
    if (ret == ESP_OK) {
        int sent = esp_websocket_client_send_text(s_ws.client,
                                                  start_json,
                                                  (int)start_len,
                                                  pdMS_TO_TICKS(LLM_GATEWAY_WS_SEND_TIMEOUT_MS));
        llm_gateway_protocol_free(start_json);
        if (sent < 0) {
            ret = ESP_FAIL;
        }
    }
    if (ret != ESP_OK) {
        (void)llm_gateway_ws_stop();
    } else if (APP_DEBUG_LLM_GATEWAY_WS) {
        ESP_LOGI(TAG, "ASR WebSocket connected");
    }

    return ret;
}

esp_err_t llm_gateway_ws_send_pcm16(const int16_t *pcm, size_t samples, uint32_t sample_rate_hz)
{
    if (pcm == NULL || samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sample_rate_hz != LLM_GATEWAY_AUDIO_SAMPLE_RATE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ws.client == NULL || !s_ws.connected || !esp_websocket_client_is_connected(s_ws.client)) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t bytes = samples * sizeof(int16_t);
    if (bytes > INT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    int sent = esp_websocket_client_send_bin(s_ws.client,
                                             (const char *)pcm,
                                             (int)bytes,
                                             pdMS_TO_TICKS(LLM_GATEWAY_WS_SEND_TIMEOUT_MS));
    if (sent < 0) {
        return ESP_FAIL;
    }
    if (APP_DEBUG_LLM_GATEWAY_AUDIO) {
        ESP_LOGI(TAG, "ASR WS PCM sent: samples=%u bytes=%u", (unsigned int)samples, (unsigned int)bytes);
    }
    return ESP_OK;
}

esp_err_t llm_gateway_ws_finish(void)
{
    if (s_ws.client == NULL || !s_ws.connected ||
        s_ws.asr_model == NULL || s_ws.asr_model[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    char *finish_json = NULL;
    size_t finish_len = 0;
    esp_err_t ret = llm_gateway_protocol_build_asr_ws_finish_event(s_ws.asr_model, &finish_json, &finish_len);
    if (ret != ESP_OK) {
        return ret;
    }

    int sent = esp_websocket_client_send_text(s_ws.client,
                                              finish_json,
                                              (int)finish_len,
                                              pdMS_TO_TICKS(LLM_GATEWAY_WS_SEND_TIMEOUT_MS));
    llm_gateway_protocol_free(finish_json);
    return sent < 0 ? ESP_FAIL : ESP_OK;
}

esp_err_t llm_gateway_ws_stop(void)
{
    if (s_ws.client != NULL) {
        if (esp_websocket_client_is_connected(s_ws.client)) {
            (void)esp_websocket_client_stop(s_ws.client);
        }
        (void)esp_websocket_client_destroy(s_ws.client);
    }
    llm_gateway_ws_clear_rx_buffer();
    if (s_ws.event_group != NULL) {
        vEventGroupDelete(s_ws.event_group);
    }
    memset(&s_ws, 0, sizeof(s_ws));
    return ESP_OK;
}

bool llm_gateway_ws_is_connected(void)
{
    return s_ws.client != NULL &&
           s_ws.connected &&
           esp_websocket_client_is_connected(s_ws.client);
}
