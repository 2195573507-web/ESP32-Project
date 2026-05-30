#ifndef BME690_LLM_BRIDGE_H
#define BME690_LLM_BRIDGE_H

#include "esp_err.h"

/**
 * @file bme690_llm_bridge.h
 * @brief BME690 传感器上下文到统一 llm_client 的桥接层。
 *
 * 调用方法：BME690 模块只调用本文件，不直接依赖 llm_client 或网关协议细节。
 * 当前阶段仅提供 JSON 上下文入口，不主动采集传感器。
 */

/* BME690 模型配置：模型名归属 BME690 bridge，llm 层只负责转发到网关。 */
#define BME690_LLM_BRIDGE_LLM_MODEL            "请填入BME690_LLM模型名"           // BME690 传感器上下文使用的 LLM 模型。

/**
 * @brief 初始化 BME690 LLM bridge。
 *
 * 调用方法：系统初始化时可调用一次；当前只保留桥接入口。
 *
 * @return 成功返回 ESP_OK。
 */
esp_err_t bme690_llm_bridge_init(void);

/**
 * @brief 发送 BME690 JSON 上下文到 LLM。
 *
 * @param json BME690 JSON 文本，不能为空。
 * @return 成功返回 ESP_OK；参数错误或 LLM 请求失败时返回错误码。
 */
esp_err_t bme690_llm_bridge_send_json(const char *json);

/**
 * @brief 按 BME690 读数生成 JSON 并发送到 LLM。
 *
 * @param temperature 温度。
 * @param humidity 湿度。
 * @param pressure 气压。
 * @param gas_resistance 气体电阻。
 * @return 成功返回 ESP_OK；JSON 缓冲不足或 LLM 请求失败时返回错误码。
 */
esp_err_t bme690_llm_bridge_send_reading(float temperature,
                                         float humidity,
                                         float pressure,
                                         float gas_resistance);

#endif // BME690_LLM_BRIDGE_H
