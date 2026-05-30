#ifndef LLM_GATEWAY_WS_H
#define LLM_GATEWAY_WS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * @file llm_gateway_ws.h
 * @brief 豆包边缘网关 ASR WebSocket streaming 封装。
 *
 * 调用方法：只允许 llm_client 调用本文件接口。上层 bridge 不直接依赖官方
 * esp_websocket_client、WebSocket 事件或 ASR streaming JSON。
 */

typedef enum {
    LLM_GATEWAY_WS_EVENT_CONNECTED = 0,  // WebSocket 已连接。
    LLM_GATEWAY_WS_EVENT_DISCONNECTED,   // WebSocket 已断开。
    LLM_GATEWAY_WS_EVENT_ASR_PARTIAL,    // ASR partial 文本。
    LLM_GATEWAY_WS_EVENT_ASR_FINAL,      // ASR final 文本。
    LLM_GATEWAY_WS_EVENT_TTS_AUDIO,      // TTS 音频占位，当前不播放。
    LLM_GATEWAY_WS_EVENT_ERROR,          // WebSocket 或服务端错误。
} llm_gateway_ws_event_type_t;

typedef struct {
    llm_gateway_ws_event_type_t type; // 事件类型。
    const char *text;                 // ASR 文本，可为空。
    size_t audio_len;                 // TTS 音频字节数，当前仅记录。
    int code;                         // 错误码或服务端状态码。
    const char *message;              // 错误/状态说明，可为空。
} llm_gateway_ws_event_t;

typedef void (*llm_gateway_ws_event_cb_t)(const llm_gateway_ws_event_t *event, void *user_ctx);

typedef struct {
    const char *asr_model;              // ASR 模型名，不能为空。
    llm_gateway_ws_event_cb_t event_cb; // WebSocket 事件回调，可为空。
    void *user_ctx;                     // 回调用户上下文。
} llm_gateway_ws_config_t;

/**
 * @brief 启动 ASR WebSocket streaming 会话。
 *
 * 调用方法：llm_client_start_voice_session() 内部调用。函数会拼接 WS URL、
 * 设置 Authorization Header，并发送 session.start JSON。
 *
 * @param config WebSocket 配置，不能为空；asr_model 不能为空。
 * @return 成功返回 ESP_OK；建连、鉴权或 session.start 失败时返回错误码。
 */
esp_err_t llm_gateway_ws_start(const llm_gateway_ws_config_t *config);

/**
 * @brief 向当前 ASR WebSocket 会话发送 PCM16 音频。
 *
 * 调用方法：Mic 采集任务经 mic_llm_bridge/llm_client 转发到这里。
 *
 * @param pcm PCM16 样本指针，不能为空。
 * @param samples PCM 样本数，必须大于 0。
 * @param sample_rate_hz PCM 采样率。
 * @return 成功返回 ESP_OK；未连接、参数错误或发送失败时返回错误码。
 */
esp_err_t llm_gateway_ws_send_pcm16(const int16_t *pcm, size_t samples, uint32_t sample_rate_hz);

/**
 * @brief 发送 ASR streaming 结束事件。
 *
 * 调用方法：Mic VAD 触发 VOICE_END 后由 llm_client 调用。
 *
 * @return 成功返回 ESP_OK；未连接或发送失败时返回错误码。
 */
esp_err_t llm_gateway_ws_finish(void);

/**
 * @brief 停止 ASR WebSocket 会话并释放资源。
 *
 * 调用方法：识别完成、异常或需要中断当前会话时调用。
 *
 * @return 成功返回 ESP_OK。
 */
esp_err_t llm_gateway_ws_stop(void);

/**
 * @brief 查询 ASR WebSocket 是否已连接。
 *
 * @return 已连接返回 true，否则返回 false。
 */
bool llm_gateway_ws_is_connected(void);

#endif // LLM_GATEWAY_WS_H
