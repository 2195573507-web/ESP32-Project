#include "ai_bridge_common.h"

#include <stdarg.h>
#include <stdio.h>

#include "esp_log.h"

static const char *TAG = "ai_bridge_common";

esp_err_t ai_bridge_check_str(const char *s, const char *name)
{
    if (s == NULL || s[0] == '\0') {
        ESP_LOGW(TAG, "%s is empty", name != NULL ? name : "string");
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t ai_bridge_check_out_buf(char *buf, size_t size, const char *name)
{
    if (buf == NULL || size == 0) {
        ESP_LOGW(TAG, "%s buffer invalid", name != NULL ? name : "output");
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = '\0';
    return ESP_OK;
}

esp_err_t ai_bridge_appendf(char *buf,
                            size_t buf_size,
                            size_t *offset,
                            const char *fmt,
                            ...)
{
    if (buf == NULL || buf_size == 0 || offset == NULL || fmt == NULL || *offset >= buf_size) {
        return ESP_ERR_INVALID_ARG;
    }

    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(buf + *offset, buf_size - *offset, fmt, args);
    va_end(args);
    if (written < 0 || (size_t)written >= buf_size - *offset) {
        return ESP_ERR_INVALID_SIZE;
    }
    *offset += (size_t)written;
    return ESP_OK;
}
