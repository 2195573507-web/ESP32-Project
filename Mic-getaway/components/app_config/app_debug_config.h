#ifndef APP_DEBUG_CONFIG_H
#define APP_DEBUG_CONFIG_H

#include "esp_log.h"

/**
 * @file app_debug_config.h
 * @brief 项目统一调试开关配置。
 *
 * 调用方法：
 * 1. 需要排查 Mic / 网关 ASR / LLM 时，只修改本文件里的 APP_DEBUG_* 宏。
 * 2. 普通运行保持默认值即可：关键状态和错误日志常显，高噪声 hex / payload / PCM
 *    每包统计默认关闭。
 * 3. 各模块头文件只保留业务参数，实际调试开关值统一来自这里。
 */

/* Mic ADC 调试：循环与栈水位日志默认关闭，错误日志不受这些开关影响。 */
#define APP_DEBUG_MIC_ADC_LOOP_LOG                 0  // ADC 采样循环普通诊断日志。
#define APP_DEBUG_MIC_ADC_STACK_LOG                0  // ADC 任务栈水位诊断日志。

/* ASR 会话关键日志：默认只保留主流程一句话日志，细节由调试开关打开。 */
#define APP_DEBUG_ASR_SESSION_LOG                  0  // ASR session 细节日志。
#define APP_DEBUG_ASR_FINAL_LOG                    0  // ASR final 以外的详细文本日志。
#define APP_DEBUG_ASR_VAD_KEY_LOG                  0  // VAD 起止/post-roll 细节日志。
#define APP_DEBUG_ASR_VAD_STATE_LOG                0  // silence_ms 等连续状态日志，默认关闭避免串口刷屏。

/* ASR/VAD 默认参数：集中在这里方便现场调试，不改变 WiFi、ADC 或 PCM 转换链路。 */
#define APP_ASR_AUDIO_PACKET_MS                    100  // 网关 ASR 音频包时长，当前 100 ms。
#define APP_ASR_VAD_SPEECH_START_RMS              1000 // 约 1000~1100，达到该 RMS 认为开始说话。
#define APP_ASR_VAD_SPEECH_END_RMS                850  // 低于该 RMS 累计静音，认为接近说话结束。
#define APP_ASR_VAD_SILENCE_END_MS                1500 // 静音累计 1500 ms 后结束本轮音频。
#define APP_ASR_VAD_MIN_RECORD_MS                 800  // 防止过短语音误触发结束。
#define APP_ASR_VAD_MAX_RECORD_MS                 8000 // 最长 8 s，避免异常时持续发送。
#define APP_ASR_PRE_SPEECH_PACKETS                5    // 句首预缓存 5 个 100 ms ASR 包。
#define APP_ASR_POST_ROLL_MS                      700  // VOICE_END 后继续发送 700 ms PCM，再 finalize。

/* PCM 音质调试：默认关闭，排查静音或削波时再打开。 */
#define APP_DEBUG_ASR_PCM_PACKET_STATS_LOG         0  // pcm_min/max/avg/rms/zero_cross 等质量统计。
#define APP_DEBUG_ASR_PCM_EVERY_PACKET_STATS       0  // 1 表示每包统计；0 表示按间隔统计。
#define APP_DEBUG_ASR_PCM_HEX_DUMP                 0  // 每个音频包前 N 字节 PCM hex，默认关闭。

/* 火山引擎边缘大模型网关调试：默认只打印关键状态，不打印 PCM 原始数据或明文 API Key。 */
#define APP_DEBUG_LLM_CLIENT                       0  // llm_client 状态机和收尾细节。
#define APP_DEBUG_LLM_GATEWAY_HTTP                 0  // HTTP status/body preview 等细节。
#define APP_DEBUG_LLM_GATEWAY_WS                   0  // WebSocket 建连、普通事件和关闭细节。
#define APP_DEBUG_LLM_GATEWAY_PROTO                0  // 网关 JSON 协议组装/解析摘要。
#define APP_DEBUG_LLM_GATEWAY_AUDIO                0  // 音频 chunk 尺寸日志；不打印 PCM 原始数据。

/* Speaker/IIS 调试：默认关闭，不影响 Mic/ASR 主链路串口输出。 */
#define APP_DEBUG_BSP_IIS_LOG                    0  // IIS/PDM 底层 GPIO、DMA 配置诊断。
#define APP_DEBUG_SPEAKER_PLAYER_LOG             0  // speaker 播放器 ringbuffer/DMA/heap 诊断。

/* LLM bridge 调试：默认关闭桥接细节，具体 payload 仍由上面的网关开关控制。 */
#define APP_DEBUG_MIC_LLM_BRIDGE                   0  // Mic bridge 语音起止和 llm_client 事件细节。
#define APP_DEBUG_SPEAKER_LLM_BRIDGE               0  // speaker bridge/TTS 合成调用日志。
#define APP_DEBUG_SCREEN_BRIDGE                    0  // screen bridge/LCD 命令日志。
#define APP_DEBUG_BME690_LLM_BRIDGE                0  // BME690 bridge 传感器上下文调用。
#define APP_DEBUG_CSI_LLM_BRIDGE                   0  // CSI bridge 传感器上下文调用。
#define APP_DEBUG_SYSTEM_LLM_BRIDGE                0  // system bridge 状态 JSON。

/* 调试预览长度和节流参数：只影响日志大小，不改变 ASR 协议或音频发送内容。 */
#define APP_DEBUG_ASR_PCM_HEX_PREVIEW_BYTES        16  // PCM hex 预览字节数。
#define APP_DEBUG_ASR_PCM_STATS_INTERVAL_PACKETS   100 // PCM 质量统计默认每 100 包打印一次。

#if APP_DEBUG_ASR_PCM_HEX_PREVIEW_BYTES < 0
#error "APP_DEBUG_ASR_PCM_HEX_PREVIEW_BYTES must not be negative"
#endif

#if APP_DEBUG_ASR_PCM_STATS_INTERVAL_PACKETS <= 0
#error "APP_DEBUG_ASR_PCM_STATS_INTERVAL_PACKETS must be greater than 0"
#endif

#if APP_ASR_AUDIO_PACKET_MS <= 0
#error "APP_ASR_AUDIO_PACKET_MS must be greater than 0"
#endif

#if APP_ASR_VAD_SPEECH_START_RMS <= 0
#error "APP_ASR_VAD_SPEECH_START_RMS must be greater than 0"
#endif

#if APP_ASR_VAD_SPEECH_END_RMS < 0
#error "APP_ASR_VAD_SPEECH_END_RMS must not be negative"
#endif

#if APP_ASR_VAD_SILENCE_END_MS <= 0
#error "APP_ASR_VAD_SILENCE_END_MS must be greater than 0"
#endif

#if APP_ASR_VAD_MIN_RECORD_MS < 0
#error "APP_ASR_VAD_MIN_RECORD_MS must not be negative"
#endif

#if APP_ASR_VAD_MAX_RECORD_MS <= 0
#error "APP_ASR_VAD_MAX_RECORD_MS must be greater than 0"
#endif

#if APP_ASR_VAD_MAX_RECORD_MS < APP_ASR_VAD_MIN_RECORD_MS
#error "APP_ASR_VAD_MAX_RECORD_MS must be greater than or equal to APP_ASR_VAD_MIN_RECORD_MS"
#endif

#if APP_ASR_PRE_SPEECH_PACKETS < 3 || APP_ASR_PRE_SPEECH_PACKETS > 5
#error "APP_ASR_PRE_SPEECH_PACKETS must be between 3 and 5"
#endif

#if APP_ASR_POST_ROLL_MS < 0 || APP_ASR_POST_ROLL_MS > 1000
#error "APP_ASR_POST_ROLL_MS must be between 0 and 1000"
#endif

/**
 * @brief 应用默认日志等级。
 *
 * 调用方法：app_main() 最早调用一次。普通运行保持底层库只输出 warn/error，
 * 项目自己的主流程 INFO 仍可见；需要细节时打开上面的 APP_DEBUG_* 宏。
 */
static inline void app_debug_apply_log_levels(void)
{
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("wifi_init", ESP_LOG_WARN);
    esp_log_level_set("phy_init", ESP_LOG_WARN);
    esp_log_level_set("pp", ESP_LOG_WARN);
    esp_log_level_set("net80211", ESP_LOG_WARN);
    esp_log_level_set("esp_netif_handlers", ESP_LOG_WARN);
    esp_log_level_set("esp-tls", ESP_LOG_WARN);
    esp_log_level_set("transport_base", ESP_LOG_WARN);
    esp_log_level_set("transport_ws", ESP_LOG_WARN);
    esp_log_level_set("websocket_client", ESP_LOG_WARN);
    esp_log_level_set("esp_http_client", ESP_LOG_WARN);
}

#endif // APP_DEBUG_CONFIG_H
