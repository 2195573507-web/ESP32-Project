#ifndef CSI_LLM_BRIDGE_H
#define CSI_LLM_BRIDGE_H

#include "esp_err.h"

/**
 * @file csi_llm_bridge.h
 * @brief CSI 传感器上下文到统一 llm_client 的桥接层。
 *
 * 调用方法：CSI 模块只调用本文件，不直接依赖 llm_client 或网关协议细节。
 * 当前阶段仅提供 JSON/summary 上下文入口，不主动采集 CSI。
 */

/* CSI 模型配置：模型名归属 CSI bridge，llm 层只负责转发到网关。 */
#define CSI_LLM_BRIDGE_LLM_MODEL               "请填入CSI_LLM模型名"              // CSI 上下文使用的 LLM 模型。

/**
 * @brief 初始化 CSI LLM bridge。
 *
 * 调用方法：系统初始化时可调用一次；当前只保留桥接入口。
 *
 * @return 成功返回 ESP_OK。
 */
esp_err_t csi_llm_bridge_init(void);

/**
 * @brief 发送 CSI JSON 上下文到 LLM。
 *
 * @param json CSI JSON 文本，不能为空。
 * @return 成功返回 ESP_OK；参数错误或 LLM 请求失败时返回错误码。
 */
esp_err_t csi_llm_bridge_send_json(const char *json);

/**
 * @brief 发送 CSI 摘要 JSON 到 LLM。
 *
 * @param summary_json CSI 摘要 JSON 文本，不能为空。
 * @return 成功返回 ESP_OK；参数错误或 LLM 请求失败时返回错误码。
 */
esp_err_t csi_llm_bridge_send_summary(const char *summary_json);

#endif // CSI_LLM_BRIDGE_H
