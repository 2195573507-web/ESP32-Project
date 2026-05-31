#ifndef LLM_CONFIG_H
#define LLM_CONFIG_H

#include "app_debug_config.h"
#include "volc_gateway_config.h"

/**
 * @file llm_config.h
 * @brief 火山引擎边缘大模型网关兼容配置。
 *
 * 调用方法：业务模块不要直接依赖 HTTP/WebSocket 或网关 JSON 细节，只通过各自
 * bridge 调用 llm_client。HTTP、WebSocket、OpenAI 兼容 JSON 和返回解析
 * 都封装在 llm 目录内部。
 */

/* 模型能力配置：bridge 只选择能力，不直接写具体模型名。 */
#define LLM_GATEWAY_ASR_MODEL                  VOLC_GATEWAY_ASR_MODEL              // ASR Realtime 模型。
#define LLM_GATEWAY_TEXT_MODEL                 VOLC_GATEWAY_CHAT_MODEL             // 文本理解/命令决策模型。
#define LLM_GATEWAY_TTS_MODEL                  "legacy-tts-disabled"               // TTS 模型仅 legacy 占位，当前禁用。

/* Mic 音频格式：必须与 mic_adc_test / mic_adc_pcm 的 PCM 输出保持一致。 */
#define LLM_GATEWAY_AUDIO_SAMPLE_RATE          VOLC_GATEWAY_ASR_SAMPLE_RATE        // PCM 采样率：16 kHz。
#define LLM_GATEWAY_AUDIO_BITS                 VOLC_GATEWAY_ASR_BITS               // PCM 位深：signed int16。
#define LLM_GATEWAY_AUDIO_CHANNELS             VOLC_GATEWAY_ASR_CHANNELS           // 单声道。
#define LLM_GATEWAY_AUDIO_FORMAT               VOLC_GATEWAY_ASR_FORMAT             // Realtime API input_audio_format。
#define LLM_GATEWAY_AUDIO_CODEC                VOLC_GATEWAY_ASR_CODEC              // Realtime API input_audio_codec。

/* ASR 策略：当前只走 WebSocket Realtime streaming。 */
#define LLM_GATEWAY_ASR_USE_STREAMING          1                                  // 1 表示优先使用 ASR WebSocket streaming。

/* 功能开关：当前阶段跑 Mic -> ASR -> Chat，不实现 TTS 播放。 */
#define LLM_GATEWAY_USE_OFFICIAL_WS            1                                  // 使用 espressif/esp_websocket_client。
#define LLM_GATEWAY_ENABLE_ASR                 1                                  // ASR 能力开关。
#define LLM_GATEWAY_ENABLE_TEXT                1                                  // 文本理解/命令决策能力开关。
#define LLM_GATEWAY_ENABLE_ASR_TO_CHAT         1                                  // ASR final 文本返回 ESP 后自动发送给 Chat。
#define LLM_GATEWAY_ENABLE_TTS                 0                                  // TTS 总开关，当前必须保持关闭。

/* 超时与缓冲参数：只影响网关请求等待时间和本地文本缓存大小。 */
#define LLM_GATEWAY_HTTP_TIMEOUT_MS            20000                              // HTTP Chat 请求超时。
#define LLM_GATEWAY_WS_CONNECT_TIMEOUT_MS      10000                              // WebSocket 建连等待超时。
#define LLM_GATEWAY_WS_FINAL_TIMEOUT_MS        5000                               // ASR streaming 等待 final 兜底超时。
#define LLM_GATEWAY_WS_DRAIN_QUIET_MS          300                                // 收到 final 后继续等待一小段静默，尽量 drain 服务端事件。
#define LLM_GATEWAY_WS_SEND_TIMEOUT_MS         5000                               // WebSocket 单次发送超时。
#define LLM_GATEWAY_CHAT_TASK_STACK_SIZE       12288                              // ASR final 后发起 Chat 的任务栈。
#define LLM_GATEWAY_CHAT_TASK_PRIORITY         4                                  // Chat 任务优先级，避免压过 WiFi 系统任务。
#define LLM_GATEWAY_WS_BUFFER_BYTES            12288                              // 100 ms PCM base64 JSON 发送缓冲。
#define LLM_GATEWAY_ASR_BASE64_BUFFER_BYTES    5000                               // 100 ms PCM base64 临时缓冲。
#define LLM_GATEWAY_ASR_JSON_BUFFER_BYTES      8192                               // ASR append JSON 临时缓冲。
#define LLM_GATEWAY_HTTP_RESPONSE_MAX_BYTES    4096                               // HTTP 响应 JSON 缓冲大小。
#define LLM_GATEWAY_LLM_RESPONSE_MAX_BYTES     2048                               // LLM final 文本缓冲大小。
#define LLM_GATEWAY_ASR_TEXT_MAX_BYTES         512                                // ASR 文本缓冲大小。
#define LLM_GATEWAY_SENSOR_CONTEXT_MAX_BYTES   1024                               // 传感器上下文 JSON 缓冲上限。

/* 配置检查与鉴权参数：构建 Authorization Header 时统一使用 Bearer 前缀。 */
#define LLM_GATEWAY_PLACEHOLDER_MARKER         "请填入"                            // 占位符检测关键字。
#define LLM_GATEWAY_AUTH_BEARER_PREFIX         "Bearer "                          // Authorization Header 前缀。

/* LLM 系统提示词：Mic ASR final 和显式 Chat 查询都会使用。 */
#define LLM_GATEWAY_SYSTEM_PROMPT              "你是运行在ESP32设备上的中文决策模型。请根据输入上下文给出简短、结构化、可执行的回答。"

#if LLM_GATEWAY_AUDIO_SAMPLE_RATE != 16000
#error "LLM_GATEWAY_AUDIO_SAMPLE_RATE must stay at 16000 for the current Mic PCM path"
#endif

#if LLM_GATEWAY_AUDIO_BITS != 16
#error "LLM_GATEWAY_AUDIO_BITS must stay at 16 for signed PCM16"
#endif

#if LLM_GATEWAY_AUDIO_CHANNELS != 1
#error "LLM_GATEWAY_AUDIO_CHANNELS must stay mono for the current Mic PCM path"
#endif

#endif // LLM_CONFIG_H
