#ifndef LLM_CONFIG_H
#define LLM_CONFIG_H

#include "app_debug_config.h"

/**
 * @file llm_config.h
 * @brief LLM / 火山引擎边缘大模型网关集中配置。
 *
 * 调用方法：业务模块不要直接依赖 HTTP/WebSocket 或网关 JSON 细节，只通过各自
 * bridge 调用 llm_client。HTTP、WebSocket、OpenAI 兼容 JSON 和返回解析
 * 都封装在 llm 目录内部。本文件是 llm 目录唯一真实配置源。
 */

/* 网关模式总开关：当前项目主链路使用火山引擎边缘大模型网关。 */
#define LLM_CLIENT_USE_VOLC_GATEWAY             1                                  // 1 表示启用网关模式。

/* 网关鉴权：API Key 只允许放在本配置文件；日志只能打印长度和脱敏摘要。 */
#define VOLC_GATEWAY_API_KEY                    "sk-12d89cfd90bd4846b3f2ed399540cea75bjjw6shymxhmm86"        // 网关 API Key，禁止明文打印。

/* 网关地址：主链路代码只能使用下面组合出来的 URI，不要散落写域名或 path。 */
#define VOLC_GATEWAY_WS_BASE_URL                "wss://ai-gateway.vei.volces.com"  // WebSocket 网关根地址。
#define VOLC_GATEWAY_HTTP_BASE_URL              "https://ai-gateway.vei.volces.com" // HTTP 网关根地址。
#define VOLC_GATEWAY_REALTIME_PATH              "/v1/realtime"                     // Realtime ASR/TTS path。
#define VOLC_GATEWAY_CHAT_PATH                  "/v1/chat/completions"             // Chat Completions path。

/* 模型名集中配置：bridge 只选择能力，不直接写模型字符串。 */
#define VOLC_GATEWAY_ASR_MODEL                  "bigmodel"                         // Realtime ASR 模型。
#define LLM_CHAT_MODEL                          "Doubao-Seed-1.6-flash"            // Chat Completions 文本理解/命令决策模型。
#define VOLC_GATEWAY_CHAT_MODEL                 LLM_CHAT_MODEL                     // 兼容旧命名，Chat 请求统一使用 LLM_CHAT_MODEL。
#define VOLC_GATEWAY_TTS_MODEL                  "doubao-tts"                       // Realtime TTS 模型。

#define VOLC_GATEWAY_ASR_REALTIME_URI           VOLC_GATEWAY_WS_BASE_URL VOLC_GATEWAY_REALTIME_PATH "?model=" VOLC_GATEWAY_ASR_MODEL // ASR Realtime 完整 URI。
#define VOLC_GATEWAY_CHAT_COMPLETIONS_URI       VOLC_GATEWAY_HTTP_BASE_URL VOLC_GATEWAY_CHAT_PATH // Chat Completions 完整 URI。
#define VOLC_GATEWAY_TTS_REALTIME_URI           VOLC_GATEWAY_WS_BASE_URL VOLC_GATEWAY_REALTIME_PATH "?model=" VOLC_GATEWAY_TTS_MODEL // TTS Realtime 完整 URI。

/* Mic ASR 音频参数：必须与 mic_adc_pcm 输出保持一致。 */
#define VOLC_GATEWAY_ASR_SAMPLE_RATE            16000                              // PCM 采样率：16 kHz。
#define VOLC_GATEWAY_ASR_BITS                   16                                 // PCM 位深：signed int16。
#define VOLC_GATEWAY_ASR_CHANNELS               1                                  // 单声道。
#define VOLC_GATEWAY_ASR_FORMAT                 "pcm"                              // Realtime input_audio_format。
#define VOLC_GATEWAY_ASR_CODEC                  "raw"                              // Realtime input_audio_codec。

/* Resource ID：平台预置 Doubao ASR 默认不发送；自有三方渠道再打开。 */
#define VOLC_GATEWAY_USE_RESOURCE_ID            0                                  // 0 表示不发送 X-Api-Resource-Id。
#define VOLC_GATEWAY_ASR_RESOURCE_ID            "volc.bigasr.sauc.duration"        // legacy/三方渠道资源 ID，占位保留。

/* TTS 输出参数：当前只启用网关合成能力，播放由 speaker 底层后续接入。 */
#define VOLC_GATEWAY_TTS_VOICE                  "zh_female_kailangjiejie_moon_bigtts" // 默认中文音色。
#define VOLC_GATEWAY_TTS_OUTPUT_FORMAT          "pcm"                              // 输出 PCM，便于后续 I2S 播放。
#define VOLC_GATEWAY_TTS_OUTPUT_SAMPLE_RATE     16000                              // 输出采样率：16 kHz。
#define VOLC_GATEWAY_TTS_OUTPUT_CHANNELS        1                                  // 单声道。
#define VOLC_GATEWAY_TTS_USE_RESOURCE_ID        0                                  // 平台预置 doubao-tts 默认不发送 Resource ID。
#define VOLC_GATEWAY_TTS_RESOURCE_ID            "volc.service_type.10029"          // 自有三方渠道 TTS Resource ID，占位保留。

/* LLM 内部能力映射：bridge 只选择能力，不直接写具体模型名。 */
#define LLM_GATEWAY_ASR_MODEL                  VOLC_GATEWAY_ASR_MODEL              // ASR Realtime 模型。
#define LLM_GATEWAY_TEXT_MODEL                 LLM_CHAT_MODEL                      // 文本理解/命令决策模型。
#define LLM_GATEWAY_TTS_MODEL                  VOLC_GATEWAY_TTS_MODEL              // TTS Realtime 模型。

/* Mic 音频格式：必须与 mic_adc_test / mic_adc_pcm 的 PCM 输出保持一致。 */
#define LLM_GATEWAY_AUDIO_SAMPLE_RATE          VOLC_GATEWAY_ASR_SAMPLE_RATE        // PCM 采样率：16 kHz。
#define LLM_GATEWAY_AUDIO_BITS                 VOLC_GATEWAY_ASR_BITS               // PCM 位深：signed int16。
#define LLM_GATEWAY_AUDIO_CHANNELS             VOLC_GATEWAY_ASR_CHANNELS           // 单声道。
#define LLM_GATEWAY_AUDIO_FORMAT               VOLC_GATEWAY_ASR_FORMAT             // Realtime API input_audio_format。
#define LLM_GATEWAY_AUDIO_CODEC                VOLC_GATEWAY_ASR_CODEC              // Realtime API input_audio_codec。

/* ASR 策略：当前只走 WebSocket Realtime streaming。 */
#define LLM_GATEWAY_ASR_USE_STREAMING          1                                  // 1 表示优先使用 ASR WebSocket streaming。

/* 功能开关：Mic 主链路跑 Mic -> ASR -> Chat；TTS 仅显式调用，不自动播放。 */
#define LLM_GATEWAY_USE_OFFICIAL_WS            1                                  // 使用 espressif/esp_websocket_client。
#define LLM_GATEWAY_ENABLE_ASR                 1                                  // ASR 能力开关。
#define LLM_GATEWAY_ENABLE_TEXT                1                                  // 文本理解/命令决策能力开关。
#define LLM_GATEWAY_ENABLE_ASR_TO_CHAT         1                                  // ASR final 文本返回 ESP 后自动发送给 Chat。
#define LLM_GATEWAY_ENABLE_TTS                 1                                  // TTS 能力开关；只允许 speaker bridge 显式调用。

/* 超时与缓冲参数：只影响网关请求等待时间和本地文本缓存大小。 */
#define LLM_GATEWAY_HTTP_TIMEOUT_MS            20000                              // HTTP Chat 请求超时。
#define LLM_GATEWAY_WS_CONNECT_TIMEOUT_MS      10000                              // WebSocket 建连等待超时。
#define LLM_GATEWAY_WS_FINAL_TIMEOUT_MS        5000                               // ASR streaming 等待 final 兜底超时。
#define LLM_GATEWAY_WS_DRAIN_QUIET_MS          300                                // 收到 final 后继续等待一小段静默，尽量 drain 服务端事件。
#define LLM_GATEWAY_WS_SEND_TIMEOUT_MS         5000                               // WebSocket 单次发送超时。
#define LLM_GATEWAY_TTS_FINAL_TIMEOUT_MS       10000                              // TTS 等待 response.audio.done 超时。
#define LLM_GATEWAY_CHAT_TASK_STACK_SIZE       12288                              // ASR final 后发起 Chat 的任务栈。
#define LLM_GATEWAY_CHAT_TASK_PRIORITY         4                                  // Chat 任务优先级，避免压过 WiFi 系统任务。
#define LLM_GATEWAY_WS_BUFFER_BYTES            12288                              // 100 ms PCM base64 JSON 发送缓冲。
#define LLM_GATEWAY_TTS_WS_PAYLOAD_MAX_BYTES   65536                              // TTS 单帧 JSON payload 上限。
#define LLM_GATEWAY_TTS_AUDIO_CHUNK_MAX_BYTES  32768                              // TTS 单个 base64 delta 解码音频上限。
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
#define LLM_GATEWAY_SYSTEM_PROMPT              "你是 ESP32-C5 设备上的语音助手，请简洁回答。"

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
