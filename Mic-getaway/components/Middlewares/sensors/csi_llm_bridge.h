#ifndef CSI_LLM_BRIDGE_H
#define CSI_LLM_BRIDGE_H

#include <stddef.h>

#include "esp_err.h"

/**
 * @file csi_llm_bridge.h
 * @brief CSI 传感器上下文到统一 llm_client 的桥接层。
 *
 * 调用方法：CSI 模块只调用本文件，不直接依赖 llm_client 或网关协议细节。
 * 当前阶段仅提供 JSON/summary 上下文入口，不主动采集 CSI。
 */

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

typedef struct {
    float rssi;             // RSSI，单位 dBm。
    float noise_floor;      // 噪声底，单位 dBm。
    float activity_score;   // 活动分数。
    int presence_detected;  // 是否检测到人体存在。
    int motion_detected;    // 是否检测到运动。
} ai_csi_summary_t;

esp_err_t ai_csi_bridge_init(void);
esp_err_t ai_csi_bridge_report(const ai_csi_summary_t *summary);
esp_err_t ai_csi_bridge_ask(const ai_csi_summary_t *summary,
                            const char *question,
                            char *out_reply,
                            size_t out_reply_size);
esp_err_t ai_csi_bridge_build_context(const ai_csi_summary_t *summary,
                                      char *out_context,
                                      size_t out_context_size);

#endif // CSI_LLM_BRIDGE_H
