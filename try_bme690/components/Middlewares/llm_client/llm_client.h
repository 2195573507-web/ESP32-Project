#ifndef LLM_CLIENT_H
#define LLM_CLIENT_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

/**
 * @brief 初始化大模型客户端
 *
 * 此函数初始化大模型客户端相关配置。
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t llm_client_init(void);

/**
 * @brief 发送消息到大模型并获取响应
 *
 * 此函数发送用户消息到大模型API，并返回响应。
 * @param message 用户消息
 * @param response_buffer 存储响应的缓冲区
 * @param buffer_size 缓冲区大小
 * @param response_len 返回的响应长度
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t llm_send_message(const char *message, char *response_buffer, size_t buffer_size, size_t *response_len);

/**
 * @brief 设置大模型API的URL
 *
 * 此函数设置大模型API的端点URL。
 * @param url API URL
 */
void llm_set_api_url(const char *url);

/**
 * @brief 设置API密钥
 *
 * 此函数设置访问大模型API的密钥。
 * @param api_key API密钥
 */
void llm_set_api_key(const char *api_key);

#endif // LLM_CLIENT_H