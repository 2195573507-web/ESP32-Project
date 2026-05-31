#ifndef VOLC_GATEWAY_AUTH_H
#define VOLC_GATEWAY_AUTH_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/**
 * @file volc_gateway_auth.h
 * @brief 火山引擎网关 Authorization Header 公共封装。
 *
 * 调用方法：gateway HTTP/WebSocket 只能通过本文件生成 Header；业务模块和 bridge
 * 不直接读取 API Key，也不直接拼接 Authorization。
 */

/**
 * @brief 构造 Authorization Header value。
 *
 * @param out 输出缓冲区，不能为空。
 * @param out_size out 字节数，必须大于 0。
 * @return 成功返回 ESP_OK；缓冲不足或参数错误时返回错误码。
 */
esp_err_t volc_gateway_auth_build_authorization(char *out, size_t out_size);

/**
 * @brief 构造 esp_websocket_client 可注入的 Header 字符串。
 *
 * @param out 输出缓冲区，不能为空。
 * @param out_size out 字节数，必须大于 0。
 * @return 成功返回 ESP_OK；缓冲不足或参数错误时返回错误码。
 */
esp_err_t volc_gateway_auth_build_ws_headers(char *out, size_t out_size);

/**
 * @brief 构造 TTS Realtime WebSocket Header 字符串。
 *
 * 调用方法：TTS 网关连接时调用。平台预置 TTS 模型默认只注入
 * Authorization；自有三方渠道才按配置追加 X-Api-Resource-Id。
 *
 * @param out 输出缓冲区，不能为空。
 * @param out_size out 字节数，必须大于 0。
 * @return 成功返回 ESP_OK；缓冲不足或参数错误时返回错误码。
 */
esp_err_t volc_gateway_auth_build_tts_ws_headers(char *out, size_t out_size);

/**
 * @brief 生成 API Key 脱敏摘要。
 *
 * @param out 输出摘要缓冲区，不能为空。
 * @param out_size out 字节数，必须大于 0。
 */
void volc_gateway_auth_make_key_summary(char *out, size_t out_size);

/**
 * @brief 判断 API Key 是否仍为占位符。
 *
 * @return 包含占位符返回 true，否则返回 false。
 */
bool volc_gateway_auth_has_placeholder(void);

#endif // VOLC_GATEWAY_AUTH_H
