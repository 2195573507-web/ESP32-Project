#ifndef VOLC_GATEWAY_CONFIG_H
#define VOLC_GATEWAY_CONFIG_H

/**
 * @file volc_gateway_config.h
 * @brief 火山引擎边缘大模型网关集中配置。
 *
 * 调用方法：网关 URL、模型名、API Key 和音频参数只允许从本文件导出。
 * 业务模块、bridge、llm_client 和 transport 代码都不要硬编码这些值。
 */

/* 网关模式总开关：当前项目主链路使用火山引擎边缘大模型网关。 */
#define LLM_CLIENT_USE_VOLC_GATEWAY             1                                  // 1 表示启用网关模式。

/* 网关鉴权：API Key 只允许放在本配置文件；日志只能打印长度和脱敏摘要。 */
#define VOLC_GATEWAY_API_KEY                    "sk-12d89cfd90bd4846b3f2ed399540cea75bjjw6shymxhmm86"        // 网关 API Key，禁止明文打印。

/* 网关地址：主链路代码只能使用下面组合出来的 URI，不要散落写域名或 path。 */
#define VOLC_GATEWAY_WS_BASE_URL                "wss://ai-gateway.vei.volces.com"  // WebSocket 网关根地址。
#define VOLC_GATEWAY_HTTP_BASE_URL              "https://fai-gateway.vei.volces.com" // HTTP 网关根地址。
#define VOLC_GATEWAY_REALTIME_PATH              "/v1/realtime"                     // Realtime ASR path。
#define VOLC_GATEWAY_CHAT_PATH                  "/v1/chat/completions"             // Chat Completions path。

/* 模型名集中配置：bridge 只选择能力，不直接写模型字符串。 */
#define VOLC_GATEWAY_ASR_MODEL                  "bigmodel"                         // Realtime ASR 模型。
#define VOLC_GATEWAY_CHAT_MODEL                 "Doubao-Seed-1.6-flash"            // 文本理解/命令决策模型。

#define VOLC_GATEWAY_ASR_REALTIME_URI           VOLC_GATEWAY_WS_BASE_URL VOLC_GATEWAY_REALTIME_PATH "?model=" VOLC_GATEWAY_ASR_MODEL // ASR Realtime 完整 URI。
#define VOLC_GATEWAY_CHAT_COMPLETIONS_URI       VOLC_GATEWAY_HTTP_BASE_URL VOLC_GATEWAY_CHAT_PATH // Chat Completions 完整 URI。

/* Mic ASR 音频参数：必须与 mic_adc_pcm 输出保持一致。 */
#define VOLC_GATEWAY_ASR_SAMPLE_RATE            16000                              // PCM 采样率：16 kHz。
#define VOLC_GATEWAY_ASR_BITS                   16                                 // PCM 位深：signed int16。
#define VOLC_GATEWAY_ASR_CHANNELS               1                                  // 单声道。
#define VOLC_GATEWAY_ASR_FORMAT                 "pcm"                              // Realtime input_audio_format。
#define VOLC_GATEWAY_ASR_CODEC                  "raw"                              // Realtime input_audio_codec。

/* Resource ID：平台预置 Doubao ASR 默认不发送；自有三方渠道再打开。 */
#define VOLC_GATEWAY_USE_RESOURCE_ID            0                                  // 0 表示不发送 X-Api-Resource-Id。
#define VOLC_GATEWAY_ASR_RESOURCE_ID            "volc.bigasr.sauc.duration"        // legacy/三方渠道资源 ID，占位保留。

#endif // VOLC_GATEWAY_CONFIG_H
