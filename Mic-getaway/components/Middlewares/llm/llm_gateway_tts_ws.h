#ifndef LLM_GATEWAY_TTS_WS_H
#define LLM_GATEWAY_TTS_WS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * @file llm_gateway_tts_ws.h
 * @brief 火山引擎边缘网关 TTS Realtime WebSocket 封装。
 *
 * 调用方法：只允许 llm_client 调用本文件接口。speaker bridge 不直接依赖
 * WebSocket、TTS JSON 事件或 Authorization Header。
 */

typedef enum {
    LLM_GATEWAY_TTS_WS_EVENT_CONNECTED = 0,      // TTS WebSocket 已连接。
    LLM_GATEWAY_TTS_WS_EVENT_SESSION_UPDATED,    // TTS session.update 已生效。
    LLM_GATEWAY_TTS_WS_EVENT_AUDIO_DELTA,        // 收到一段解码后的 TTS 音频。
    LLM_GATEWAY_TTS_WS_EVENT_AUDIO_DONE,         // 本轮 TTS 音频输出完成。
    LLM_GATEWAY_TTS_WS_EVENT_DISCONNECTED,       // TTS WebSocket 已断开。
    LLM_GATEWAY_TTS_WS_EVENT_ERROR,              // TTS WebSocket 或服务端错误。
} llm_gateway_tts_ws_event_type_t;

typedef struct {
    llm_gateway_tts_ws_event_type_t type; // 事件类型。
    uint8_t *audio;                       // 音频数据，仅 AUDIO_DELTA 有效；audio_owned 为 true 时由回调接管。
    size_t audio_len;                     // audio 字节数。
    bool audio_owned;                     // true 表示 audio 指针 ownership 已转交给回调。
    int code;                             // 错误码或服务端状态码。
    const char *message;                  // 错误/状态说明，可为空。
} llm_gateway_tts_ws_event_t;

typedef void (*llm_gateway_tts_ws_event_cb_t)(const llm_gateway_tts_ws_event_t *event, void *user_ctx);

typedef struct {
    const char *tts_model;                  // TTS 模型名，不能为空。
    llm_gateway_tts_ws_event_cb_t event_cb; // WebSocket 事件回调，可为空。
    void *user_ctx;                         // 回调用户上下文。
} llm_gateway_tts_ws_config_t;

/**
 * @brief 执行一次文本到语音合成。
 *
 * 调用方法：llm_client_tts_text() 内部调用。函数会建立 Realtime WebSocket、
 * 发送 tts_session.update、input_text.append、input_text.done，接收
 * response.audio.delta，并在完成或超时后关闭连接。
 *
 * @param config TTS WebSocket 配置，不能为空；tts_model 不能为空。
 * @param text 待合成文本，不能为空。
 * @return 成功收到 response.audio.done 返回 ESP_OK；连接、鉴权、协议或超时失败返回错误码。
 */
esp_err_t llm_gateway_tts_ws_synthesize(const llm_gateway_tts_ws_config_t *config,
                                        const char *text);

#endif // LLM_GATEWAY_TTS_WS_H
