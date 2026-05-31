#ifndef AI_BRIDGE_COMMON_H
#define AI_BRIDGE_COMMON_H

#include <stddef.h>

#include "esp_err.h"

/**
 * @file ai_bridge_common.h
 * @brief AI bridge 公共参数检查和安全字符串拼接工具。
 */

esp_err_t ai_bridge_check_str(const char *s, const char *name);
esp_err_t ai_bridge_check_out_buf(char *buf, size_t size, const char *name);
esp_err_t ai_bridge_appendf(char *buf,
                            size_t buf_size,
                            size_t *offset,
                            const char *fmt,
                            ...);

#endif // AI_BRIDGE_COMMON_H
