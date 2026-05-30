#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <stddef.h>

#include "esp_err.h"

/**
 * @file http_client.h
 * @brief 跨工程通用阻塞式 HTTP/HTTPS 客户端薄封装。
 *
 * 本模块只封装 ESP-IDF esp_http_client 的 GET/POST 基础调用，供 llm_client 等
 * 通用中间件复用；业务层不应在这里写入 WiFi、main 或具体云服务逻辑。
 */

/* HTTP 默认参数：业务层可通过 http_set_timeout() 修改超时。 */
#define HTTP_CLIENT_DEFAULT_TIMEOUT_MS      10000             // 默认 10 秒阻塞等待超时。
#define HTTP_CLIENT_CONTENT_TYPE_JSON       "application/json" // 默认 POST JSON Content-Type。

#if HTTP_CLIENT_DEFAULT_TIMEOUT_MS <= 0
#error "HTTP_CLIENT_DEFAULT_TIMEOUT_MS must be greater than 0"
#endif

/**
 * @brief 初始化 HTTP 客户端封装。
 *
 * 调用方法：上层模块初始化时调用。本实现没有常驻连接，只保留默认超时配置。
 *
 * @return 成功返回 ESP_OK。
 */
esp_err_t http_client_init(void);

/**
 * @brief 发送阻塞式 HTTP GET 请求。
 *
 * @param url 请求 URL，支持 http/https。
 * @param response_buffer 输出响应体缓冲区，函数会写入 '\0' 结尾。
 * @param buffer_size response_buffer 字节数。
 * @param response_len 输出响应体长度。
 * @return 成功返回 ESP_OK；建连、读取或缓冲不足时返回错误码。
 */
esp_err_t http_get(const char *url, char *response_buffer, size_t buffer_size, size_t *response_len);

/**
 * @brief 发送阻塞式 HTTP POST 请求。
 *
 * @param url 请求 URL，支持 http/https。
 * @param post_data POST body。
 * @param data_len POST body 字节数。
 * @param response_buffer 输出响应体缓冲区，函数会写入 '\0' 结尾。
 * @param buffer_size response_buffer 字节数。
 * @param response_len 输出响应体长度。
 * @return 成功返回 ESP_OK；HTTP 状态非 200、读取失败或缓冲不足时返回错误码。
 */
esp_err_t http_post(const char *url, const char *post_data, size_t data_len, char *response_buffer, size_t buffer_size, size_t *response_len);

/**
 * @brief 发送带一个额外 Header 的阻塞式 HTTP POST 请求。
 *
 * 调用方法：llm_client 用它添加 Authorization Header。Content-Type 会固定为
 * HTTP_CLIENT_CONTENT_TYPE_JSON。
 */
esp_err_t http_post_with_headers(const char *url, const char *post_data, size_t data_len,
                                 const char *header_name, const char *header_value,
                                 char *response_buffer, size_t buffer_size, size_t *response_len);

/**
 * @brief 设置 HTTP 请求超时时间。
 *
 * @param timeout_ms 超时时间，单位毫秒。
 */
void http_set_timeout(int timeout_ms);

#endif // HTTP_CLIENT_H
