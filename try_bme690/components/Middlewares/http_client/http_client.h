#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

/**
 * @brief 初始化HTTP客户端
 *
 * 此函数初始化HTTP客户端相关配置。
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t http_client_init(void);

/**
 * @brief 发送HTTP GET请求
 *
 * 此函数发送GET请求到指定的URL，并返回响应数据。
 * @param url 请求的URL
 * @param response_buffer 存储响应的缓冲区
 * @param buffer_size 缓冲区大小
 * @param response_len 返回的响应长度
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t http_get(const char *url, char *response_buffer, size_t buffer_size, size_t *response_len);

/**
 * @brief 发送HTTP POST请求
 *
 * 此函数发送POST请求到指定的URL，包含请求体，并返回响应数据。
 * @param url 请求的URL
 * @param post_data POST请求的数据
 * @param data_len 数据长度
 * @param response_buffer 存储响应的缓冲区
 * @param buffer_size 缓冲区大小
 * @param response_len 返回的响应长度
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t http_post(const char *url, const char *post_data, size_t data_len, char *response_buffer, size_t buffer_size, size_t *response_len);

esp_err_t http_post_with_headers(const char *url, const char *post_data, size_t data_len,
                                 const char *header_name, const char *header_value,
                                 char *response_buffer, size_t buffer_size, size_t *response_len);

/**
 * @brief 设置HTTP请求的超时时间
 *
 * 此函数设置HTTP请求的超时时间（毫秒）。
 * @param timeout_ms 超时时间
 */
void http_set_timeout(int timeout_ms);

#endif // HTTP_CLIENT_H