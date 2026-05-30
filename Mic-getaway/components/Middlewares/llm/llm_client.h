#ifndef LLM_CLIENT_H
#define LLM_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "llm_config.h"

/**
 * @file llm_client.h
 * @brief 全项目唯一底层 AI 网关客户端入口。
 *
 * 调用方法：Mic、speaker、BME690、CSI、系统状态等模块只能通过各自 bridge
 * 调用本文件公开 API，不允许直接依赖网关 HTTP、WebSocket 或协议细节。
 */

typedef enum {
    LLM_CLIENT_EVENT_CONNECTED = 0,   // 网关连接成功。
    LLM_CLIENT_EVENT_DISCONNECTED,    // 网关连接断开或会话停止。
    LLM_CLIENT_EVENT_ASR_PARTIAL_TEXT, // ASR partial 文本，仅用于日志展示。
    LLM_CLIENT_EVENT_ASR_FINAL_TEXT,   // ASR final 文本，会继续进入 LLM。
    LLM_CLIENT_EVENT_LLM_DELTA_TEXT,   // LLM 增量文本占位，当前未启用流式 LLM。
    LLM_CLIENT_EVENT_LLM_FINAL_TEXT,   // LLM final 文本。
    LLM_CLIENT_EVENT_COMMAND_RESULT,   // router 已处理 command/speech 结果。
    LLM_CLIENT_EVENT_TTS_AUDIO,        // TTS 音频占位，当前忽略。
    LLM_CLIENT_EVENT_ERROR,            // 网关、解析或 router 错误。
} llm_client_event_type_t;

typedef struct {
    llm_client_event_type_t type; // 事件类型。
    const char *text;             // 文本事件内容，可为空。
    const uint8_t *audio;         // 音频事件数据，当前 TTS 禁用时为空。
    size_t audio_len;             // audio 字节数。
    int code;                     // 错误码或服务端状态码。
    const char *message;          // 错误/状态说明，可为空。
} llm_client_event_t;

typedef void (*llm_client_event_cb_t)(const llm_client_event_t *event, void *user_ctx);

typedef struct {
    const char *asr_model;            // 当前 bridge 使用的 ASR 模型名，不能为空。
    const char *llm_model;            // 当前 bridge 使用的 LLM 模型名，不能为空。
    const char *tts_model;            // 当前 bridge 使用的 TTS 模型名，可为空。
    const char *system_prompt;        // LLM system prompt；为空时使用 LLM_GATEWAY_SYSTEM_PROMPT。
    llm_client_event_cb_t event_cb;   // 事件回调，可为空。
    void *user_ctx;                   // 回调用户上下文。
} llm_client_config_t;

/**
 * @brief 初始化统一 LLM 网关客户端。
 *
 * 调用方法：WiFi 准备好前后均可调用一次；bridge 层负责持有初始化状态。
 *
 * @param config 初始化配置，不能为空。
 * @return 成功返回 ESP_OK；配置仍是占位符或参数错误时返回错误码。
 */
esp_err_t llm_client_init(const llm_client_config_t *config);

/**
 * @brief 反初始化统一 LLM 网关客户端。
 *
 * 调用方法：需要关闭当前语音会话并释放 fallback 录音缓存时调用。
 *
 * @return 成功返回 ESP_OK。
 */
esp_err_t llm_client_deinit(void);

/**
 * @brief 开始一次语音会话。
 *
 * 调用方法：Mic VAD 触发 VOICE_START 后调用。默认启动 ASR WebSocket streaming，
 * 并准备一份 PCM fallback 缓存。
 *
 * @return 成功返回 ESP_OK；WebSocket 建连失败且不允许 fallback 时返回错误码。
 */
esp_err_t llm_client_start_voice_session(void);

/**
 * @brief 发送一段 PCM16 音频到当前语音会话。
 *
 * 调用方法：Mic 采集任务持续调用。pcm 必须是 signed int16、单声道 PCM，
 * sample_rate_hz 必须等于 LLM_GATEWAY_AUDIO_SAMPLE_RATE。
 *
 * @param pcm PCM16 样本指针，不能为空。
 * @param samples 样本数，必须大于 0。
 * @param sample_rate_hz PCM 采样率。
 * @return 成功返回 ESP_OK；会话未启动、参数错误或发送失败时返回错误码。
 */
esp_err_t llm_client_send_audio_pcm16(const int16_t *pcm, size_t samples, uint32_t sample_rate_hz);

/**
 * @brief 结束当前语音会话并进入 ASR final -> LLM -> router 链路。
 *
 * 调用方法：Mic VAD 触发 VOICE_END 后调用一次。若 streaming 没有 final 且允许
 * HTTP fallback，会使用本地缓存的 PCM 再走一次 HTTP ASR。
 *
 * @return 成功返回 ESP_OK；ASR/LLM/router 任一阶段失败时返回错误码。
 */
esp_err_t llm_client_finish_voice_session(void);

/**
 * @brief 停止当前语音会话并释放资源。
 *
 * 调用方法：异常、超时或需要中断当前识别时调用。
 *
 * @return 成功返回 ESP_OK。
 */
esp_err_t llm_client_stop_voice_session(void);

/**
 * @brief 查询当前是否存在活动语音会话。
 *
 * @return 有活动语音会话返回 true，否则返回 false。
 */
bool llm_client_is_voice_session_active(void);

/**
 * @brief 直接发送文本到 LLM chat。
 *
 * 调用方法：非语音入口或 bridge 需要直接提问时调用；返回文本会继续进入 router。
 *
 * @param user_text 用户文本，不能为空。
 * @return 成功返回 ESP_OK；HTTP 请求或 router 失败时返回错误码。
 */
esp_err_t llm_client_chat_text(const char *user_text);

/**
 * @brief 使用指定模型直接发送文本到 LLM chat。
 *
 * 调用方法：各 bridge 需要使用本模块专属模型时调用；返回文本会继续进入 router。
 *
 * @param llm_model LLM 模型名，不能为空。
 * @param user_text 用户文本，不能为空。
 * @return 成功返回 ESP_OK；HTTP 请求或 router 失败时返回错误码。
 */
esp_err_t llm_client_chat_text_with_model(const char *llm_model, const char *user_text);

/**
 * @brief 发送 JSON 上下文到 LLM chat。
 *
 * 调用方法：传感器/系统 bridge 需要把结构化上下文交给 LLM 时调用。
 *
 * @param json_context JSON 文本，不能为空。
 * @return 成功返回 ESP_OK；参数错误或 LLM 请求失败时返回错误码。
 */
esp_err_t llm_client_chat_json_context(const char *json_context);

/**
 * @brief 使用指定模型发送 JSON 上下文到 LLM chat。
 *
 * 调用方法：传感器/系统 bridge 需要使用本模块专属模型时调用。
 *
 * @param llm_model LLM 模型名，不能为空。
 * @param json_context JSON 文本，不能为空。
 * @return 成功返回 ESP_OK；参数错误或 LLM 请求失败时返回错误码。
 */
esp_err_t llm_client_chat_json_context_with_model(const char *llm_model, const char *json_context);

/**
 * @brief 发送传感器 JSON 上下文到 LLM。
 *
 * @param source 传感器来源名，不能为空。
 * @param json 传感器 JSON，不能为空。
 * @return 成功返回 ESP_OK；参数错误或 LLM 请求失败时返回错误码。
 */
esp_err_t llm_client_send_sensor_json(const char *source, const char *json);

/**
 * @brief 使用指定模型发送传感器 JSON 上下文到 LLM。
 *
 * 调用方法：各传感器 bridge 应优先调用本函数并传入本模块自己的模型名宏。
 *
 * @param source 传感器来源名，不能为空。
 * @param json 传感器 JSON，不能为空。
 * @param llm_model LLM 模型名，不能为空。
 * @return 成功返回 ESP_OK；参数错误或 LLM 请求失败时返回错误码。
 */
esp_err_t llm_client_send_sensor_json_with_model(const char *source, const char *json, const char *llm_model);

/**
 * @brief 发送 BME690 JSON 上下文到 LLM。
 *
 * @param json BME690 JSON 文本，不能为空。
 * @return 成功返回 ESP_OK；参数错误或 LLM 请求失败时返回错误码。
 */
esp_err_t llm_client_send_bme690_json(const char *json);

/**
 * @brief 发送 CSI JSON 上下文到 LLM。
 *
 * @param json CSI JSON 文本，不能为空。
 * @return 成功返回 ESP_OK；参数错误或 LLM 请求失败时返回错误码。
 */
esp_err_t llm_client_send_csi_json(const char *json);

/**
 * @brief 发送系统状态 JSON 上下文到 LLM。
 *
 * @param json 系统状态 JSON 文本，不能为空。
 * @return 成功返回 ESP_OK；参数错误或 LLM 请求失败时返回错误码。
 */
esp_err_t llm_client_send_system_status_json(const char *json);

/**
 * @brief TTS 文本接口占位。
 *
 * 调用方法：当前阶段 TTS 禁用，本函数返回 ESP_ERR_NOT_SUPPORTED，不播放音频。
 *
 * @param text 待播报文本，不能为空。
 * @return 当前固定返回 ESP_ERR_NOT_SUPPORTED。
 */
esp_err_t llm_client_tts_text(const char *text);

/**
 * @brief 查询 TTS 是否启用。
 *
 * @return 当前 LLM_GATEWAY_ENABLE_TTS 为 0，返回 false。
 */
bool llm_client_is_tts_enabled(void);

#endif // LLM_CLIENT_H
