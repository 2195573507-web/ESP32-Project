#ifndef MIC_ASR_DOUBAO_H
#define MIC_ASR_DOUBAO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "app_debug_config.h"

/**
 * @file mic_asr_doubao.h
 * @brief 豆包 ASR WebSocket 二进制业务协议层。
 *
 * 本模块只负责豆包 ASR 业务：组装 X-Api-* Header、发送豆包二进制协议帧、
 * 按 PCM 分包发送音频、解析服务端 result.text。底层 WebSocket/TLS/RFC6455
 * 帧处理已经拆到 manual_ws 模块。
 */

/* WebSocket 业务入口：manual_ws 只接收 host/path/port，不写死这些豆包地址。 */
#define MIC_ASR_DOUBAO_WS_SCHEME             "wss"                                                 // 仅用于日志和 URI 展示。
#define MIC_ASR_DOUBAO_WS_HOST               "openspeech.bytedance.com"                            // 豆包 ASR 主机名。
#define MIC_ASR_DOUBAO_WS_PORT               443                                                   // wss TLS 端口。
#define MIC_ASR_DOUBAO_WS_PATH               "/api/v3/sauc/bigmodel"                               // HTTP Upgrade request-target。
#define MIC_ASR_DOUBAO_WS_URI                "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel" // 安全日志用完整 URI。

/* 豆包 X-Api-* 业务 Header：由 mic_asr_doubao.c 组装后通过 extra_headers 传给 manual_ws。 */
#define MIC_ASR_DOUBAO_APP_KEY               "1240212599"                                           // X-Api-App-Key，日志必须脱敏。
#define MIC_ASR_DOUBAO_ACCESS_KEY            "2XEvwJo3fMNnqlHi40TtJwp6iC7xJ88L"                     // X-Api-Access-Key，敏感凭据。
#define MIC_ASR_DOUBAO_RESOURCE_ID           "volc.bigasr.sauc.duration"                            // X-Api-Resource-Id。
#define MIC_ASR_DOUBAO_CONNECT_ID            ""                                                     // 空值表示每次自动生成 UUID。

/* ASR 音频与请求参数：同步写入 full client request JSON 和 PCM 分包逻辑。 */
#define MIC_ASR_DOUBAO_SAMPLE_RATE           16000                                                  // PCM 采样率：16 kHz。
#define MIC_ASR_DOUBAO_BITS                  16                                                     // PCM 位深：int16。
#define MIC_ASR_DOUBAO_CHANNELS              1                                                      // 单声道。
#define MIC_ASR_DOUBAO_PACKET_MS             APP_ASR_AUDIO_PACKET_MS                                // 每包音频 100 ms。
#define MIC_ASR_DOUBAO_BYTES_PER_SAMPLE      (MIC_ASR_DOUBAO_BITS / 8)                              // int16 PCM 每样本 2 字节。
#define MIC_ASR_DOUBAO_PACKET_BYTES          ((MIC_ASR_DOUBAO_SAMPLE_RATE * MIC_ASR_DOUBAO_CHANNELS * MIC_ASR_DOUBAO_BYTES_PER_SAMPLE * MIC_ASR_DOUBAO_PACKET_MS) / 1000) // 16 kHz/16 bit/mono 下 100 ms = 3200 字节。
#define MIC_ASR_DOUBAO_FIRST_AUDIO_SEQUENCE  2                                                      // FULL_CLIENT_REQUEST/服务端首个响应占用 sequence=1，首个 AUDIO_ONLY_REQUEST 必须从 2 开始。
#define MIC_ASR_DOUBAO_LANGUAGE              "zh-CN"                                                // 识别语言。
#define MIC_ASR_DOUBAO_MODEL_NAME            "bigmodel"                                             // 豆包模型名。

/* 运行参数：首包后先短读一次，音频发送期间边发边收，last packet 后再阻塞等待最终文本。 */
#define MIC_ASR_DOUBAO_POST_FULL_RECV_TIMEOUT_MS   300   // FULL_CLIENT_REQUEST 后先等待一次服务端 ACK/错误；超时继续发送音频。
#define MIC_ASR_DOUBAO_SEND_POLL_TIMEOUT_MS        10    // 每个音频包发送后非阻塞短轮询服务端返回帧。
#define MIC_ASR_DOUBAO_PACKET_SEND_DELAY_MS        MIC_ASR_DOUBAO_PACKET_MS // 每发一个 100 ms PCM 包后节流 100 ms，避免过快灌包。
#define MIC_ASR_DOUBAO_RESPONSE_TIMEOUT_MS         15000 // last packet 后等待 result.text。
#define MIC_ASR_DOUBAO_RESULT_TEXT_MAX_LEN         256   // result.text 输出缓冲区大小。
#define MIC_ASR_DOUBAO_VAD_SPEECH_START_RMS        APP_ASR_VAD_SPEECH_START_RMS // 本地 RMS VAD：达到该 rms 认为开始说话。
#define MIC_ASR_DOUBAO_VAD_SILENCE_END_RMS         APP_ASR_VAD_SPEECH_END_RMS   // 本地 RMS VAD：低于该 rms 认为当前 100 ms 包是静音。
#define MIC_ASR_DOUBAO_VAD_SILENCE_END_MS          APP_ASR_VAD_SILENCE_END_MS   // IN_SPEECH 后累计静音达到 800 ms 主动结束音频流。
#define MIC_ASR_DOUBAO_VAD_MIN_RECORD_MS           APP_ASR_VAD_MIN_RECORD_MS    // 最短录音时长；说话开始后未达到该时长不触发静音结束。
#define MIC_ASR_DOUBAO_VAD_MAX_RECORD_MS           APP_ASR_VAD_MAX_RECORD_MS    // 最长录音时长；达到后主动发送 LAST_AUDIO_ONLY_REQUEST。
#define MIC_ASR_DOUBAO_ENABLE_START_DEBUG_LOG       APP_DEBUG_ASR_START_DIAG_LOG          // 启动 URI/heap 诊断，调建连时改 app_debug_config.h。
#define MIC_ASR_DOUBAO_ENABLE_FRAME_DEBUG_LOG       APP_DEBUG_ASR_WS_FRAME_LOG            // 旧版 WebSocket 粗粒度帧日志，调协议时改 app_debug_config.h。
#define MIC_ASR_DOUBAO_ENABLE_RESULT_PRINT          APP_DEBUG_ASR_LEGACY_RESULT_PRINT     // 旧版结果 ESP_LOG/printf；正常只看 ASR FINAL。
#define MIC_ASR_DOUBAO_ENABLE_INTERIM_LOG           APP_DEBUG_ASR_INTERIM_LOG             // interim 文本变化时打印 ASR INTERIM；不会触发下游 LLM。
#define MIC_ASR_DOUBAO_ENABLE_RESULT_JSON_DEBUG_LOG APP_DEBUG_ASR_RESULT_JSON_SUMMARY_LOG // 打印服务端 duration/result.text 摘要，默认关闭。
#define MIC_ASR_DOUBAO_ENABLE_PCM_QUALITY_LOG       APP_DEBUG_ASR_PCM_PACKET_STATS_LOG    // PCM 质量统计日志；需要排查音频质量时打开。
#define MIC_ASR_DOUBAO_DEBUG_PCM_SEND_SIZE          APP_DEBUG_ASR_PCM_SEND_SIZE_LOG       // PCM 发包尺寸/sequence 日志；需要排查 sequence 时打开。
#define MIC_ASR_DOUBAO_ENABLE_PCM_SEND_HEX_DUMP     APP_DEBUG_ASR_PCM_HEX_DUMP            // 每个音频包前 N 字节 PCM hex，默认关闭。
#define MIC_ASR_DOUBAO_PCM_SEND_HEX_PREVIEW_BYTES   APP_DEBUG_ASR_PCM_HEX_PREVIEW_BYTES   // PCM hex 预览字节数。
#define MIC_ASR_DOUBAO_PCM_QUALITY_LOG_EVERY_PACKET APP_DEBUG_ASR_PCM_EVERY_PACKET_STATS  // 1 表示每包打印 pcm_min/pcm_max/pcm_rms；0 表示按间隔打印。
#define MIC_ASR_DOUBAO_ENABLE_VAD_STATE_LOG         APP_DEBUG_ASR_VAD_STATE_LOG           // silence_ms 等连续 VAD 状态日志，默认关闭。

/* ASR 协议级诊断参数：只打印协议头、服务端响应摘要和有限 hex，不打印 PCM 原始内容。 */
#define MIC_ASR_DOUBAO_ENABLE_PROTOCOL_DEBUG_LOG       APP_DEBUG_ASR_PROTOCOL_HEADER_LOG       // 豆包业务二进制协议头诊断；错误日志不受影响。
#define MIC_ASR_DOUBAO_ENABLE_PROTOCOL_PREFIX_DUMP     APP_DEBUG_ASR_PROTOCOL_PREFIX_DUMP      // 业务帧 prefix hex dump，高噪声默认关闭。
#define MIC_ASR_DOUBAO_SERVER_PAYLOAD_HEX_PREVIEW_BYTES APP_DEBUG_ASR_PAYLOAD_HEX_PREVIEW_BYTES // 服务端 payload hex 预览字节数。
#define MIC_ASR_DOUBAO_ENABLE_WS_PAYLOAD_HEX_DUMP      APP_DEBUG_ASR_WS_PAYLOAD_HEX_DUMP       // ASR 层 WebSocket payload hex dump，高噪声默认关闭。
#define MIC_ASR_DOUBAO_WS_PAYLOAD_HEX_PREVIEW_BYTES    APP_DEBUG_ASR_WS_PAYLOAD_HEX_PREVIEW_BYTES // WebSocket payload hex 预览字节数。

/* ASR 结果处理参数：只影响日志去重和占位回调，不改变底层 WebSocket/PCM 协议。 */
#define MIC_ASR_DOUBAO_LOG_ID_MAX_LEN                 64  // ASR FINAL 日志中 log_id 的最大保存长度。
#define MIC_ASR_DOUBAO_FINAL_DEDUP_HISTORY            8   // 最近 N 条 final utterance 去重，按 log_id/start/end/text 匹配。

/* PCM 质量统计参数：只影响日志判断，不改变 WebSocket 发送协议和音频数据。 */
#define MIC_ASR_DOUBAO_PCM_QUALITY_LOG_INTERVAL_PACKETS   APP_DEBUG_ASR_PCM_STATS_INTERVAL_PACKETS // 每 N 个实际发送的 PCM 包打印一次质量统计。
#define MIC_ASR_DOUBAO_PCM_SILENCE_P2P_THRESHOLD          96 // p2p 小于该值时认为当前包可能接近静音。
#define MIC_ASR_DOUBAO_PCM_SILENCE_WARN_CONSECUTIVE       5  // 连续 5 个低 p2p 包后提示可能接近静音。
#define MIC_ASR_DOUBAO_PCM_CLIP_NEAR_MIN                  (-32000) // 接近 int16_t 下限的削波判断阈值。
#define MIC_ASR_DOUBAO_PCM_CLIP_NEAR_MAX                  32000    // 接近 int16_t 上限的削波判断阈值。
#define MIC_ASR_DOUBAO_PCM_CLIP_WARN_PERCENT              5        // 单包接近上下限样本占比达到 5% 时提示可能削波。

#if MIC_ASR_DOUBAO_BITS != 16
#error "MIC_ASR_DOUBAO_BITS must be 16 for signed int16 little-endian PCM"
#endif

#if (MIC_ASR_DOUBAO_BITS % 8) != 0
#error "MIC_ASR_DOUBAO_BITS must be a multiple of 8"
#endif

#if MIC_ASR_DOUBAO_PACKET_MS <= 0
#error "MIC_ASR_DOUBAO_PACKET_MS must be greater than 0"
#endif

#if MIC_ASR_DOUBAO_PACKET_BYTES <= 0
#error "MIC_ASR_DOUBAO_PACKET_BYTES must be greater than 0"
#endif

#if MIC_ASR_DOUBAO_FIRST_AUDIO_SEQUENCE < 2
#error "MIC_ASR_DOUBAO_FIRST_AUDIO_SEQUENCE must be at least 2"
#endif

#if MIC_ASR_DOUBAO_POST_FULL_RECV_TIMEOUT_MS < 0
#error "MIC_ASR_DOUBAO_POST_FULL_RECV_TIMEOUT_MS must not be negative"
#endif

#if MIC_ASR_DOUBAO_SEND_POLL_TIMEOUT_MS < 0
#error "MIC_ASR_DOUBAO_SEND_POLL_TIMEOUT_MS must not be negative"
#endif

#if MIC_ASR_DOUBAO_PACKET_SEND_DELAY_MS < 0
#error "MIC_ASR_DOUBAO_PACKET_SEND_DELAY_MS must not be negative"
#endif

#if MIC_ASR_DOUBAO_VAD_SPEECH_START_RMS <= 0
#error "MIC_ASR_DOUBAO_VAD_SPEECH_START_RMS must be greater than 0"
#endif

#if MIC_ASR_DOUBAO_VAD_SILENCE_END_RMS < 0
#error "MIC_ASR_DOUBAO_VAD_SILENCE_END_RMS must not be negative"
#endif

#if MIC_ASR_DOUBAO_VAD_SILENCE_END_MS <= 0
#error "MIC_ASR_DOUBAO_VAD_SILENCE_END_MS must be greater than 0"
#endif

#if MIC_ASR_DOUBAO_VAD_MIN_RECORD_MS < 0
#error "MIC_ASR_DOUBAO_VAD_MIN_RECORD_MS must not be negative"
#endif

#if MIC_ASR_DOUBAO_VAD_MAX_RECORD_MS <= 0
#error "MIC_ASR_DOUBAO_VAD_MAX_RECORD_MS must be greater than 0"
#endif

#if MIC_ASR_DOUBAO_VAD_MAX_RECORD_MS < MIC_ASR_DOUBAO_VAD_MIN_RECORD_MS
#error "MIC_ASR_DOUBAO_VAD_MAX_RECORD_MS must be greater than or equal to MIC_ASR_DOUBAO_VAD_MIN_RECORD_MS"
#endif

#if MIC_ASR_DOUBAO_PCM_SEND_HEX_PREVIEW_BYTES < 0
#error "MIC_ASR_DOUBAO_PCM_SEND_HEX_PREVIEW_BYTES must not be negative"
#endif

#if MIC_ASR_DOUBAO_LOG_ID_MAX_LEN <= 1
#error "MIC_ASR_DOUBAO_LOG_ID_MAX_LEN must be greater than 1"
#endif

#if MIC_ASR_DOUBAO_FINAL_DEDUP_HISTORY <= 0
#error "MIC_ASR_DOUBAO_FINAL_DEDUP_HISTORY must be greater than 0"
#endif

#if MIC_ASR_DOUBAO_PCM_QUALITY_LOG_INTERVAL_PACKETS <= 0
#error "MIC_ASR_DOUBAO_PCM_QUALITY_LOG_INTERVAL_PACKETS must be greater than 0"
#endif

/**
 * @brief 开始一次豆包 ASR 流式识别。
 *
 * 调用方法：WiFi 已连接且稳定后、ADC continuous 启动前调用。函数会组装豆包 X-Api-*
 * Header，调用 manual_ws_connect() 完成 TLS WebSocket 握手，然后发送 full client
 * request 二进制帧，并按 MIC_ASR_DOUBAO_POST_FULL_RECV_TIMEOUT_MS 短读一次服务端 ACK/错误。
 *
 * @return 成功返回 ESP_OK；TLS、握手或首包发送失败返回 ESP-IDF 错误码。
 */
esp_err_t mic_asr_doubao_start(void);

/**
 * @brief 向当前 ASR 会话发送 PCM 数据。
 *
 * 调用方法：录音过程中持续调用。pcm 必须指向 16 kHz、16 bit、单声道、
 * signed int16 little-endian 的 PCM_s16le 数据，bytes 是字节数；不要传 ADC raw。
 * 函数内部会缓存不足
 * MIC_ASR_DOUBAO_PACKET_BYTES 的小片段，凑满后按豆包 audio only request 发送。
 * 为了让最后一块真实 PCM 能作为 LAST_AUDIO_ONLY_REQUEST 发送，本函数会保留
 * 最近一个 100 ms 包，直到后续 PCM 到来才把上一包按普通 AUDIO_ONLY_REQUEST 发出。
 * 每个 100 ms 包发送后会 vTaskDelay(MIC_ASR_DOUBAO_PACKET_SEND_DELAY_MS)，并用
 * MIC_ASR_DOUBAO_SEND_POLL_TIMEOUT_MS 轮询服务端返回；收到 SERVER_ERROR 会立即停止发送。
 *
 * @param pcm PCM 数据指针，不能为空。
 * @param bytes PCM 字节数，必须是 int16_t 的整数倍。
 * @return 成功返回 ESP_OK；会话未启动或发送失败时返回错误码。
 */
esp_err_t mic_asr_doubao_send_pcm(const int16_t *pcm, size_t bytes);

/**
 * @brief 结束当前 ASR 会话并等待识别文本。
 *
 * 调用方法：VAD 检测到 VOICE_END 后调用一次。函数会发送 last audio packet，然后
 * 阻塞调用 manual_ws_recv_frame() 读取服务端 binary frame，并在解析到 result.text 后返回。
 * last audio packet 必须携带负 sequence 和真实 PCM payload；没有剩余 PCM 时不会再发送
 * payload_size=0 的空结束包。
 * 如果之前发送失败或收到错误导致连接已标记 broken，本函数不再发送 last 包。
 *
 * @param text_buf 输出识别文本缓冲区，可为 NULL；为 NULL 时只打印日志。
 * @param text_buf_size text_buf 大小。
 * @return 成功返回 ESP_OK；空识别文本也属于业务成功，超时或服务端错误返回错误码。
 */
esp_err_t mic_asr_doubao_finish(char *text_buf, size_t text_buf_size);

/**
 * @brief 查询当前 ASR 会话是否已经完成 final。
 *
 * 调用方法：Mic 采集任务在流式发送 PCM 后调用。若本地 RMS VAD 已经发送
 * LAST_AUDIO_ONLY_REQUEST，且服务端已经返回 final 文本，则上层可以立即结束本轮
 * session 并回到等待下一次说话，不必继续等待外层 VAD 的 VOICE_END。
 *
 * @return 当前会话已收到 final 且没有服务端错误时返回 true。
 */
bool mic_asr_doubao_session_has_final(void);

/**
 * @brief ASR final 文本占位回调。
 *
 * 调用方法：mic_asr_doubao.c 在收到去重后的 result.utterances[].definite=true 文本时
 * 调用一次。默认实现为空，不触发云端 LLM；后续业务若需要接入 LLM，可在其它模块中
 * 提供同名强符号函数覆盖默认实现。
 *
 * @param text 已确认的 final ASR 文本，不能为空。
 */
void mic_asr_on_final_text(const char *text);

/**
 * @brief 停止当前 ASR 会话并释放资源。
 *
 * 调用方法：异常、超时、识别完成或需要中断当前识别时调用。正常连接会走 manual_ws
 * close；若发送失败已标记 broken，则只销毁 TLS 资源，不再发送 close frame。
 */
void mic_asr_doubao_stop(void);

#endif // MIC_ASR_DOUBAO_H
