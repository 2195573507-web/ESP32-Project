#ifndef LLM_CONFIG_H
#define LLM_CONFIG_H

#include "app_debug_config.h"

/**
 * @file llm_config.h
 * @brief 豆包边缘大模型网关统一配置。
 *
 * 调用方法：业务模块不要直接依赖 HTTP/WebSocket 或网关 JSON 细节，只通过各自
 * bridge 调用 llm_client。HTTP、WebSocket、OpenAI 兼容 JSON、模型路由和返回解析
 * 都封装在 llm 目录内部。
 */

/* 网关入口配置：API Key 只允许放在本文件；日志只能打印长度和脱敏摘要。 */
#define LLM_GATEWAY_HTTP_BASE_URL              "https://请填入网关HTTP域名"       // HTTP 网关根地址，不带具体 path。
#define LLM_GATEWAY_WS_BASE_URL                "wss://请填入网关WebSocket域名"    // WebSocket 网关根地址，不带具体 path。
#define LLM_GATEWAY_API_KEY                    "请填入网关API_KEY"                // 网关 API Key，禁止明文打印。

/* 网关 API path：由 llm_gateway_protocol_build_url() 与 base URL 拼接。 */
#define LLM_GATEWAY_ASR_HTTP_PATH              "/v1/audio/transcriptions"         // ASR HTTP fallback path。
#define LLM_GATEWAY_ASR_WS_PATH                "/v1/audio/transcriptions/stream"  // ASR WebSocket streaming path。
#define LLM_GATEWAY_LLM_HTTP_PATH              "/v1/chat/completions"             // LLM chat completions path。
#define LLM_GATEWAY_TTS_HTTP_PATH              "/v1/audio/speech"                 // TTS path，占位保留。

/* Mic 音频格式：必须与 mic_adc_test / mic_adc_pcm 的 PCM 输出保持一致。 */
#define LLM_GATEWAY_AUDIO_SAMPLE_RATE          16000                              // PCM 采样率：16 kHz。
#define LLM_GATEWAY_AUDIO_BITS                 16                                 // PCM 位深：signed int16。
#define LLM_GATEWAY_AUDIO_CHANNELS             1                                  // 单声道。
#define LLM_GATEWAY_AUDIO_FORMAT               "pcm16"                            // 网关侧音频格式字段。

/* ASR 策略：默认走 WebSocket streaming，同时缓存一份 PCM 供 HTTP fallback 使用。 */
#define LLM_GATEWAY_ASR_USE_STREAMING          1                                  // 1 表示优先使用 ASR WebSocket streaming。
#define LLM_GATEWAY_ASR_HTTP_FALLBACK          1                                  // streaming 失败时允许 HTTP ASR fallback。
#define LLM_GATEWAY_ASR_UPLOAD_AS_WAV          1                                  // HTTP fallback 上传 WAV；0 表示上传裸 PCM。
#define LLM_GATEWAY_USE_PSRAM_RECORD_BUFFER    1                                  // 录音 fallback 缓存优先放 PSRAM。

/* 功能开关：当前阶段只跑 Mic -> ASR -> LLM -> router，不实现 TTS 播放。 */
#define LLM_GATEWAY_USE_OFFICIAL_WS            1                                  // 使用 espressif/esp_websocket_client。
#define LLM_GATEWAY_ENABLE_TTS                 0                                  // TTS 总开关，当前必须保持关闭。
#define LLM_ROUTER_COMMAND_SKIP_TTS            1                                  // command 类型默认不触发播报。
#define LLM_ROUTER_REQUIRE_JSON_OUTPUT         1                                  // LLM 回复必须按 JSON router 解析。

/* 超时与缓冲参数：只影响网关请求等待时间和本地文本/录音缓存大小。 */
#define LLM_GATEWAY_HTTP_TIMEOUT_MS            20000                              // HTTP ASR/LLM 请求超时。
#define LLM_GATEWAY_WS_CONNECT_TIMEOUT_MS      15000                              // WebSocket 建连等待超时。
#define LLM_GATEWAY_WS_FINAL_TIMEOUT_MS        20000                              // ASR streaming 等待 final 超时。
#define LLM_GATEWAY_WS_SEND_TIMEOUT_MS         5000                               // WebSocket 单次发送超时。
#define LLM_GATEWAY_HTTP_RESPONSE_MAX_BYTES    4096                               // HTTP 响应 JSON 缓冲大小。
#define LLM_GATEWAY_LLM_RESPONSE_MAX_BYTES     2048                               // LLM final 文本缓冲大小。
#define LLM_GATEWAY_ASR_TEXT_MAX_BYTES         512                                // ASR 文本缓冲大小。
#define LLM_GATEWAY_ROUTER_REPLY_MAX_BYTES     256                                // router reply 字段缓冲大小。
#define LLM_GATEWAY_SENSOR_CONTEXT_MAX_BYTES   1024                               // 传感器上下文 JSON 缓冲上限。
#define LLM_GATEWAY_JSON_LOG_MAX_BYTES         512                                // JSON 日志安全预览上限。
#define LLM_GATEWAY_RECORD_MAX_MS              10000                              // HTTP fallback 最长缓存 10 s PCM。
#define LLM_GATEWAY_RECORD_MAX_BYTES           ((LLM_GATEWAY_AUDIO_SAMPLE_RATE * LLM_GATEWAY_AUDIO_CHANNELS * (LLM_GATEWAY_AUDIO_BITS / 8) * LLM_GATEWAY_RECORD_MAX_MS) / 1000) // 最长录音 PCM 字节数。

/* 配置检查与鉴权参数：构建 Authorization Header 时统一使用 Bearer 前缀。 */
#define LLM_GATEWAY_PLACEHOLDER_MARKER         "请填入"                            // 占位符检测关键字。
#define LLM_GATEWAY_AUTH_BEARER_PREFIX         "Bearer "                          // Authorization Header 前缀。

/* LLM 系统提示词：要求模型输出严格 JSON，方便 llm_router 区分 command/speech。 */
#define LLM_GATEWAY_SYSTEM_PROMPT              "你是运行在ESP32设备上的中文语音助手。你必须只输出严格JSON，不允许输出JSON以外的文字。格式为：{\"type\":\"command或speech\",\"command\":null或命令名,\"params\":{},\"speak\":true或false,\"reply\":\"需要显示或后续播报的文本\"}。如果用户是在控制设备、切换页面、查询传感器、调节屏幕、开始或停止采集，则type为command；如果只是普通聊天或解释问题，则type为speech。command类型默认speak=false。"

/* 旧 OpenAI 兼容宏：只给保留在磁盘上的旧文件兜底，主 CMake 路径禁止继续依赖。 */
#define OPENAI_API_KEY                         "DEPRECATED_USE_LLM_GATEWAY_API_KEY"       // 旧 API Key 宏，禁止新代码使用。
#define OPENAI_BASE_URL                        "DEPRECATED_USE_LLM_GATEWAY_HTTP_BASE_URL" // 旧 base URL 宏，禁止新代码使用。
#define OPENAI_MODEL_NAME                      "DEPRECATED_USE_BRIDGE_MODEL_MACRO"        // 旧模型名宏，禁止新代码使用。
#define OPENAI_SYSTEM_PROMPT                   "DEPRECATED_USE_LLM_GATEWAY_SYSTEM_PROMPT" // 旧 system prompt 宏，禁止新代码使用。
#define OPENAI_USER_ID                         "DEPRECATED_USE_LLM_CLIENT_BRIDGE_CONTEXT" // 旧 user id 宏，禁止新代码使用。
#define OPENAI_CHAT_COMPLETIONS_PATH           "DEPRECATED_USE_LLM_GATEWAY_LLM_HTTP_PATH" // 旧 chat path 宏，禁止新代码使用。
#define OPENAI_CHAT_COMPLETIONS_URL            "DEPRECATED_USE_LLM_GATEWAY_LLM_HTTP_PATH" // 旧 chat URL 宏，禁止新代码使用。
#define OPENAI_PLACEHOLDER_MARKER              "DEPRECATED"                              // 旧占位符宏，禁止新代码使用。

#if LLM_GATEWAY_AUDIO_SAMPLE_RATE != 16000
#error "LLM_GATEWAY_AUDIO_SAMPLE_RATE must stay at 16000 for the current Mic PCM path"
#endif

#if LLM_GATEWAY_AUDIO_BITS != 16
#error "LLM_GATEWAY_AUDIO_BITS must stay at 16 for signed PCM16"
#endif

#if LLM_GATEWAY_AUDIO_CHANNELS != 1
#error "LLM_GATEWAY_AUDIO_CHANNELS must stay mono for the current Mic PCM path"
#endif

#if LLM_GATEWAY_RECORD_MAX_BYTES <= 0
#error "LLM_GATEWAY_RECORD_MAX_BYTES must be positive"
#endif

#endif // LLM_CONFIG_H
