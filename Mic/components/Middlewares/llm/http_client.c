#include "http_client.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_crt_bundle.h"
#include <string.h>

static const char *TAG = "http_client";

static int http_timeout_ms = HTTP_CLIENT_DEFAULT_TIMEOUT_MS; // 默认请求超时，可由 http_set_timeout() 覆盖。

esp_err_t http_client_init(void)
{
    ESP_LOGI(TAG, "HTTP client initialized");
    return ESP_OK;
}

esp_err_t http_get(const char *url, char *response_buffer, size_t buffer_size, size_t *response_len)
{
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = http_timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) {
        ESP_LOGE(TAG, "Failed to fetch headers");
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    if (content_length >= buffer_size) {
        ESP_LOGE(TAG, "Response too large for buffer");
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int read_len = esp_http_client_read(client, response_buffer, buffer_size - 1);
    if (read_len < 0) {
        ESP_LOGE(TAG, "Failed to read response");
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    response_buffer[read_len] = '\0';
    *response_len = read_len;

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "HTTP GET completed, response length: %d", read_len);
    return ESP_OK;
}

esp_err_t http_post(const char *url, const char *post_data, size_t data_len, char *response_buffer, size_t buffer_size, size_t *response_len)
{
    return http_post_with_headers(url, post_data, data_len, NULL, NULL, response_buffer, buffer_size, response_len);
}

esp_err_t http_post_with_headers(const char *url, const char *post_data, size_t data_len,
                                 const char *header_name, const char *header_value,
                                 char *response_buffer, size_t buffer_size, size_t *response_len)
{
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = http_timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", HTTP_CLIENT_CONTENT_TYPE_JSON);
    if (header_name != NULL && header_value != NULL) {
        esp_http_client_set_header(client, header_name, header_value);
    }
    esp_http_client_set_post_field(client, post_data, data_len);

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP POST failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP POST status code: %d", status_code);
    if (status_code != 200) {
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int read_len = esp_http_client_read_response(client, response_buffer, buffer_size - 1);
    if (read_len < 0) {
        ESP_LOGE(TAG, "Failed to read response");
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    response_buffer[read_len] = '\0';
    *response_len = read_len;

    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "HTTP POST completed, response length: %d", read_len);
    return ESP_OK;
}

void http_set_timeout(int timeout_ms)
{
    http_timeout_ms = timeout_ms;
    ESP_LOGI(TAG, "HTTP timeout set to %d ms", timeout_ms);
}
