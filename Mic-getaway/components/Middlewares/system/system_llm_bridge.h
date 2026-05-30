#ifndef SYSTEM_LLM_BRIDGE_H
#define SYSTEM_LLM_BRIDGE_H

#include <stddef.h>

#include "esp_err.h"

/**
 * @file system_llm_bridge.h
 * @brief 系统状态到统一 llm_client 的桥接层。
 *
 * 调用方法：系统模块只调用本文件，不直接依赖 llm_client 或网关协议细节。
 * 当前阶段提供本机 heap、WiFi、语音会话和 TTS 状态 JSON。
 */

/* system 模型配置：模型名归属 system bridge，llm 层只负责转发到网关。 */
#define SYSTEM_LLM_BRIDGE_LLM_MODEL            "请填入System_LLM模型名"           // 系统状态上下文使用的 LLM 模型。

/**
 * @brief 初始化 system LLM bridge。
 *
 * 调用方法：系统初始化时可调用一次；当前只保留桥接入口。
 *
 * @return 成功返回 ESP_OK。
 */
esp_err_t system_llm_bridge_init(void);

/**
 * @brief 获取系统状态 JSON。
 *
 * @param out 输出 JSON 缓冲区，不能为空。
 * @param out_size out 字节数，必须大于 0。
 * @return 成功返回 ESP_OK；参数错误或缓冲不足时返回错误码。
 */
esp_err_t system_llm_bridge_get_status_json(char *out, size_t out_size);

/**
 * @brief 发送系统状态 JSON 到 LLM。
 *
 * 调用方法：由 router 或调试入口触发时调用，不在后台自动发送。
 *
 * @return 成功返回 ESP_OK；状态 JSON 生成或 LLM 请求失败时返回错误码。
 */
esp_err_t system_llm_bridge_send_status_to_llm(void);

#endif // SYSTEM_LLM_BRIDGE_H
