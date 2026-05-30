#ifndef APP_DEBUG_CONFIG_H
#define APP_DEBUG_CONFIG_H

/**
 * @file app_debug_config.h
 * @brief 项目统一调试开关配置。
 *
 * 调用方法：
 * 1. 需要排查 Mic / 豆包 ASR / manual_ws 时，只修改本文件里的 APP_DEBUG_* 宏。
 * 2. 普通运行保持默认值即可：关键状态和错误日志常显，高噪声 hex / payload / PCM
 *    每包统计默认关闭。
 * 3. 各模块头文件只保留旧宏名兼容映射，实际开关值统一来自这里。
 */

/* Mic ADC 调试：循环与栈水位日志默认关闭，错误日志不受这些开关影响。 */
#define APP_DEBUG_MIC_ADC_LOOP_LOG                 0  // ADC 采样循环普通诊断日志。
#define APP_DEBUG_MIC_ADC_STACK_LOG                0  // ADC 任务栈水位诊断日志。

/* ASR 会话关键日志：默认保留连接/会话、INTERIM/FINAL 和 VAD 起止等低噪声日志。 */
#define APP_DEBUG_ASR_SESSION_LOG                  1  // ASR session start/end 主流程日志；当前由代码常显保留。
#define APP_DEBUG_ASR_INTERIM_LOG                  1  // interim 文本变化时打印 ASR INTERIM。
#define APP_DEBUG_ASR_FINAL_LOG                    1  // final 文本打印 ASR FINAL；当前由代码常显保留。
#define APP_DEBUG_ASR_VAD_KEY_LOG                  1  // speech started/ended/max_record 关键 VAD 日志；当前由代码常显保留。
#define APP_DEBUG_ASR_VAD_STATE_LOG                0  // silence_ms 等连续状态日志，默认关闭避免串口刷屏。

/* ASR/VAD 默认参数：集中在这里方便现场调试，不改变 WiFi、ADC 或 PCM 转换链路。 */
#define APP_ASR_AUDIO_PACKET_MS                  100  // 豆包 ASR 音频包时长，当前 100 ms。
#define APP_ASR_VAD_SPEECH_START_RMS               1000 // 约 1000~1100，达到该 RMS 认为开始说话。
#define APP_ASR_VAD_SPEECH_END_RMS                 850  // 低于该 RMS 累计静音，认为接近说话结束。
#define APP_ASR_VAD_SILENCE_END_MS                 800  // 静音累计 800 ms 后结束本轮音频。
#define APP_ASR_VAD_MIN_RECORD_MS                  800  // 防止过短语音误触发结束。
#define APP_ASR_VAD_MAX_RECORD_MS                  8000 // 最长 8 s，避免异常时持续发送。
#define APP_ASR_PRE_SPEECH_PACKETS                 5    // 句首预缓存 5 个 100 ms ASR 包。

/* ASR 建连与 WebSocket 调试：默认关闭，只在排查 TLS/握手/帧边界时打开。 */
#define APP_DEBUG_ASR_START_DIAG_LOG               0  // ASR URI、heap、密钥脱敏摘要等建连诊断。
#define APP_DEBUG_ASR_WS_FRAME_LOG                 0  // ASR 层 WebSocket frame 元信息日志。
#define APP_DEBUG_MANUAL_WS_FRAME_LOG              0  // manual_ws 底层握手、收发和控制帧详细日志。

/* 豆包业务二进制协议调试：默认关闭，不影响 SERVER_ERROR 的 error_code/message 常显。 */
#define APP_DEBUG_ASR_PROTOCOL_HEADER_LOG          0  // 解析/发送 4 字节业务协议头字段。
#define APP_DEBUG_ASR_PROTOCOL_PREFIX_DUMP         0  // 业务帧 prefix 十六进制预览，高噪声默认关闭。
#define APP_DEBUG_ASR_SERVER_ERROR_DETAIL_LOG      0  // SERVER_ERROR 的协议字段、完整帧前缀等详细诊断。

/* ASR payload 调试：默认关闭，避免 streaming JSON / 二进制 payload 重复刷屏。 */
#define APP_DEBUG_ASR_PAYLOAD_TEXT_DUMP            0  // 服务端 payload 文本/JSON/protobuf 摘要。
#define APP_DEBUG_ASR_PAYLOAD_HEX_DUMP             0  // 服务端 payload 十六进制预览。
#define APP_DEBUG_ASR_PAYLOAD_FULL_TEXT_DUMP       0  // 全量文本 payload；当前映射到安全截断打印。
#define APP_DEBUG_ASR_WS_PAYLOAD_HEX_DUMP          0  // ASR 层 WebSocket payload 前缀 hex。
#define APP_DEBUG_ASR_RESULT_JSON_SUMMARY_LOG      0  // duration/result.text 摘要调试日志。

/* PCM 发包与音质调试：默认关闭，排查音频字节序、静音或削波时再打开。 */
#define APP_DEBUG_ASR_PCM_SEND_SIZE_LOG            0  // packet_id/sequence/flags/payload_bytes/total_sent_bytes。
#define APP_DEBUG_ASR_PCM_PACKET_STATS_LOG         0  // pcm_min/max/avg/rms/zero_cross 等质量统计。
#define APP_DEBUG_ASR_PCM_EVERY_PACKET_STATS       0  // 1 表示每包统计；0 表示按间隔统计。
#define APP_DEBUG_ASR_PCM_HEX_DUMP                 0  // 每个音频包前 N 字节 PCM hex，默认关闭。
#define APP_DEBUG_ASR_LEGACY_RESULT_PRINT          0  // 旧版 ASR result ESP_LOG/printf 兼容输出。

/* 调试预览长度和节流参数：只影响日志大小，不改变 ASR 协议或音频发送内容。 */
#define APP_DEBUG_ASR_PCM_HEX_PREVIEW_BYTES        16  // PCM hex 预览字节数。
#define APP_DEBUG_ASR_PAYLOAD_HEX_PREVIEW_BYTES    256 // 服务端业务 payload hex 预览字节数。
#define APP_DEBUG_ASR_WS_PAYLOAD_HEX_PREVIEW_BYTES 32  // WebSocket payload hex 预览字节数。
#define APP_DEBUG_ASR_PCM_STATS_INTERVAL_PACKETS   20  // PCM 质量统计默认每 20 包打印一次。
#define APP_DEBUG_ASR_PARSER_MAX_PRINT_BYTES       2048 // payload 文本安全截断打印上限。
#define APP_DEBUG_ASR_PARSER_JSON_FIELD_MAX_CHARS  128  // JSON 字段摘要最大字符数。
#define APP_DEBUG_ASR_PARSER_GZIP_MAX_OUTPUT_BYTES 4096 // gzip 调试解压输出上限。

#if APP_DEBUG_ASR_PCM_HEX_PREVIEW_BYTES < 0
#error "APP_DEBUG_ASR_PCM_HEX_PREVIEW_BYTES must not be negative"
#endif

#if APP_DEBUG_ASR_PAYLOAD_HEX_PREVIEW_BYTES <= 0
#error "APP_DEBUG_ASR_PAYLOAD_HEX_PREVIEW_BYTES must be greater than 0"
#endif

#if APP_DEBUG_ASR_WS_PAYLOAD_HEX_PREVIEW_BYTES <= 0
#error "APP_DEBUG_ASR_WS_PAYLOAD_HEX_PREVIEW_BYTES must be greater than 0"
#endif

#if APP_DEBUG_ASR_PCM_STATS_INTERVAL_PACKETS <= 0
#error "APP_DEBUG_ASR_PCM_STATS_INTERVAL_PACKETS must be greater than 0"
#endif

#if APP_DEBUG_ASR_PARSER_MAX_PRINT_BYTES <= 0
#error "APP_DEBUG_ASR_PARSER_MAX_PRINT_BYTES must be greater than 0"
#endif

#if APP_DEBUG_ASR_PARSER_JSON_FIELD_MAX_CHARS <= 0
#error "APP_DEBUG_ASR_PARSER_JSON_FIELD_MAX_CHARS must be greater than 0"
#endif

#if APP_DEBUG_ASR_PARSER_GZIP_MAX_OUTPUT_BYTES <= 0
#error "APP_DEBUG_ASR_PARSER_GZIP_MAX_OUTPUT_BYTES must be greater than 0"
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

#endif // APP_DEBUG_CONFIG_H
