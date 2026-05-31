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
    LLM_CLIENT_STATE_IDLE = 0,        // 空闲态，可启动下一轮 ASR。
    LLM_CLIENT_STATE_ASR_CONNECTING,  // 正在建立 ASR WebSocket。
    LLM_CLIENT_STATE_ASR_STREAMING,   // ASR 已连接，允许发送 PCM。
    LLM_CLIENT_STATE_ASR_FINISHING,   // 已发送 commit，等待 final 或关闭。
    LLM_CLIENT_STATE_CHAT_REQUESTING, // 显式 Chat 请求中；ASR final 自动 Chat 在后台任务中发送。
    LLM_CLIENT_STATE_TTS_REQUESTING,  // 显式 TTS 请求中；不由 Mic 主链路自动进入。
} llm_client_state_t;

typedef enum {
    LLM_CLIENT_EVENT_CONNECTED = 0,   // 网关连接成功。
    LLM_CLIENT_EVENT_DISCONNECTED,    // 网关连接断开或会话停止。
    LLM_CLIENT_EVENT_ASR_PARTIAL_TEXT, // ASR partial 文本，仅用于日志展示。
    LLM_CLIENT_EVENT_ASR_FINAL_TEXT,   // ASR final 文本，先打印/回调，再按配置自动 Chat。
    LLM_CLIENT_EVENT_LLM_DELTA_TEXT,   // LLM 增量文本占位，当前未启用流式 LLM。
    LLM_CLIENT_EVENT_LLM_FINAL_TEXT,   // LLM final 文本。
    LLM_CLIENT_EVENT_COMMAND_RESULT,   // router 已处理 command/speech 结果。
    LLM_CLIENT_EVENT_TTS_AUDIO,        // TTS 音频 chunk，可交给 speaker 底层播放。
    LLM_CLIENT_EVENT_ERROR,            // 网关、解析或 router 错误。
} llm_client_event_type_t;

typedef enum {
    LLM_CLIENT_CAP_ASR = 0, // Mic 语音识别能力，对应 LLM_GATEWAY_ASR_MODEL。
    LLM_CLIENT_CAP_TEXT,    // 文本理解/命令决策能力，对应 LLM_GATEWAY_TEXT_MODEL。
    LLM_CLIENT_CAP_TTS,     // 语音合成能力，对应 LLM_GATEWAY_TTS_MODEL。
} llm_client_capability_t;

typedef struct {
    llm_client_event_type_t type; // 事件类型。
    const char *text;             // 文本事件内容，可为空。
    const uint8_t *audio;         // 音频事件数据，仅 TTS_AUDIO 事件有效。
    size_t audio_len;             // audio 字节数。
    int code;                     // 错误码或服务端状态码。
    const char *message;          // 错误/状态说明，可为空。
} llm_client_event_t;

typedef void (*llm_client_event_cb_t)(const llm_client_event_t *event, void *user_ctx);

typedef struct {
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
 * 调用方法：需要关闭当前语音会话并释放 WebSocket 资源时调用。
 *
 * @return 成功返回 ESP_OK。
 */
esp_err_t llm_client_deinit(void);

/**
 * @brief 开始一次 ASR 会话。
 *
 * 调用方法：Mic VAD 触发 VOICE_START 后调用。默认启动 ASR WebSocket streaming。
 *
 * @return 成功返回 ESP_OK；WebSocket 建连失败时返回错误码。
 */
esp_err_t llm_client_start_asr_session(void);

/**
 * @brief 发送一段 PCM16 音频到当前语音会话。
 *
 * 调用方法：Mic 采集任务持续调用。pcm 必须是 signed int16、单声道 PCM，
 * 采样率由当前 Mic 链路固定为 LLM_GATEWAY_AUDIO_SAMPLE_RATE。
 *
 * @param pcm PCM16 样本指针，不能为空。
 * @param samples 样本数，必须大于 0。
 * @return 成功返回 ESP_OK；会话未启动、参数错误或发送失败时返回错误码。
 */
esp_err_t llm_client_send_asr_pcm(const int16_t *pcm, size_t samples);

/**
 * @brief 结束当前 ASR 会话。
 *
 * 调用方法：Mic VAD 触发 VOICE_END 后调用一次。函数会发送
 * input_audio_buffer.commit，等待 final 或超时，然后关闭本轮 WebSocket 并回到 IDLE。
 *
 * @return 收尾完成返回 ESP_OK；commit 或关闭失败时返回错误码，但状态仍会回到 IDLE。
 */
esp_err_t llm_client_finish_asr_session(void);

/**
 * @brief 停止当前语音会话并释放资源。
 *
 * 调用方法：异常、超时或需要中断当前识别时调用。
 *
 * @return 成功返回 ESP_OK。
 */
esp_err_t llm_client_cancel_asr_session(void);

/**
 * @brief 查询 llm_client 当前状态。
 *
 * @return 当前状态枚举。
 */
llm_client_state_t llm_client_get_state(void);

/**
 * @brief 获取状态名。
 *
 * @param state 状态枚举。
 * @return 状态字符串。
 */
const char *llm_client_state_name(llm_client_state_t state);

/**
 * @brief 查询当前是否存在活动语音会话。
 *
 * @return 有活动语音会话返回 true，否则返回 false。
 */
bool llm_client_is_voice_session_active(void);

/* 兼容旧 Mic bridge 命名：Mic 仍只推 ASR，ASR final 后的 Chat 由 llm_client 编排。 */
esp_err_t llm_client_start_voice_session(void);
esp_err_t llm_client_send_audio_pcm16(const int16_t *pcm, size_t samples, uint32_t sample_rate_hz);
esp_err_t llm_client_finish_voice_session(void);
esp_err_t llm_client_stop_voice_session(void);

/**
 * @brief 直接发送文本到 LLM chat。
 *
 * 调用方法：非语音入口或 bridge 需要直接提问时调用；ASR final 自动 Chat
 * 也复用同一套底层 HTTP Chat 实现。
 *
 * @param user_text 用户文本，不能为空。
 * @return 成功返回 ESP_OK；当前 Chat 未启用时返回 ESP_ERR_NOT_SUPPORTED。
 */
esp_err_t llm_client_text_request(const char *user_text);

/**
 * @brief 显式文本查询接口。
 *
 * 调用方法：BME690/CSI/System 等 bridge 后续需要 Chat 决策时主动调用。
 * Mic ASR final 的自动 Chat 由 llm_client 内部任务触发。
 */
esp_err_t llm_client_text_query(const char *system_prompt,
                                const char *user_text,
                                char *out_reply,
                                size_t out_reply_size);

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
 * @brief 发送模块 JSON 上下文到文本理解/命令决策模型。
 *
 * 调用方法：BME690、CSI、System 等模块统一调用本函数或对应便捷函数；
 * llm_client 内部固定使用 LLM_CLIENT_CAP_TEXT。
 *
 * @param source 传感器来源名，不能为空。
 * @param json 传感器 JSON，不能为空。
 * @return 成功返回 ESP_OK；参数错误或 LLM 请求失败时返回错误码。
 */
esp_err_t llm_client_json_context_request(const char *source, const char *json_context);

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
 * @brief 显式 TTS 文本合成接口。
 *
 * 调用方法：speaker bridge 需要播报时调用。函数会通过 Realtime TTS 获取
 * PCM 音频 chunk，并通过 LLM_CLIENT_EVENT_TTS_AUDIO 回调上报；当前不自动接入
 * Mic/Chat 主链路，也不直接播放。
 *
 * @param text 待播报文本，不能为空。
 * @return 成功完成合成返回 ESP_OK；未启用、非空闲、网关错误或超时返回错误码。
 */
esp_err_t llm_client_tts_text(const char *text);

/**
 * @brief 查询 TTS 是否启用。
 *
 * @return LLM_GATEWAY_ENABLE_TTS 非 0 时返回 true。
 */
bool llm_client_is_tts_enabled(void);

#endif // LLM_CLIENT_H
