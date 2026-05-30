#include "mic_asr_doubao.h"

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "manual_ws.h"
#include "mic_asr_doubao_parser.h"

static const char *TAG = "mic_asr_doubao";

enum {
    MIC_ASR_DOUBAO_PROTOCOL_HEADER_BYTES = 4,
    MIC_ASR_DOUBAO_PROTOCOL_SEQUENCE_BYTES = 4,
    MIC_ASR_DOUBAO_PROTOCOL_LENGTH_BYTES = 4,
    MIC_ASR_DOUBAO_FULL_REQUEST_PREFIX_BYTES =
        MIC_ASR_DOUBAO_PROTOCOL_HEADER_BYTES + MIC_ASR_DOUBAO_PROTOCOL_LENGTH_BYTES,
    MIC_ASR_DOUBAO_AUDIO_REQUEST_PREFIX_BYTES =
        MIC_ASR_DOUBAO_PROTOCOL_HEADER_BYTES + MIC_ASR_DOUBAO_PROTOCOL_SEQUENCE_BYTES +
        MIC_ASR_DOUBAO_PROTOCOL_LENGTH_BYTES,
    MIC_ASR_DOUBAO_FRAME_BUFFER_BYTES =
        MIC_ASR_DOUBAO_AUDIO_REQUEST_PREFIX_BYTES + MIC_ASR_DOUBAO_PACKET_BYTES,
    MIC_ASR_DOUBAO_CONNECT_ID_MAX_LEN = 48,
    MIC_ASR_DOUBAO_HEADERS_MAX_LEN = 384,
    MIC_ASR_DOUBAO_JSON_MAX_LEN = 256,
    MIC_ASR_DOUBAO_WS_PAYLOAD_HEX_PREVIEW_BYTES = 32,
    MIC_ASR_DOUBAO_RX_MESSAGE_MAX_BYTES = MANUAL_WS_MAX_PAYLOAD_SIZE,
};

/* 豆包 WebSocket 二进制协议头：全量请求、普通音频包、最后一个音频包。 */
static const uint8_t MIC_ASR_DOUBAO_HEADER_FULL_CLIENT_REQUEST[MIC_ASR_DOUBAO_PROTOCOL_HEADER_BYTES] = {
    0x11, 0x10, 0x10, 0x00
};
static const uint8_t MIC_ASR_DOUBAO_HEADER_AUDIO_ONLY_REQUEST[MIC_ASR_DOUBAO_PROTOCOL_HEADER_BYTES] = {
    0x11, 0x21, 0x00, 0x00
};
static const uint8_t MIC_ASR_DOUBAO_HEADER_LAST_AUDIO_ONLY_REQUEST[MIC_ASR_DOUBAO_PROTOCOL_HEADER_BYTES] = {
    0x11, 0x23, 0x00, 0x00
};

/**
 * @brief 已输出的 final utterance 去重记录。
 *
 * 调用方法：收到 result.utterances[].definite=true 时，用 log_id/start_time/end_time/text
 * 组成业务 key，在最近 MIC_ASR_DOUBAO_FINAL_DEDUP_HISTORY 条记录中查重。
 */
typedef struct {
    bool used;                                                     // true 表示当前槽位可用于查重。
    int32_t start_time;                                            // utterance.start_time，负数会在入库前被过滤。
    int32_t end_time;                                              // utterance.end_time，负数会在入库前被过滤。
    char log_id[MIC_ASR_DOUBAO_LOG_ID_MAX_LEN];                    // 服务端 log_id，可能来自 utterance/result/root。
    char text[MIC_ASR_DOUBAO_RESULT_TEXT_MAX_LEN];                 // final 文本。
} mic_asr_doubao_final_key_t;

/**
 * @brief 本地 RMS 端点检测状态。
 *
 * 调用方法：每个 100 ms PCM 包发送前，根据当前包 pcm_rms 推进状态机。该状态机只决定
 * 本地何时把当前包作为 LAST_AUDIO_ONLY_REQUEST 发送，不改变豆包协议字段和 PCM 数据。
 */
typedef enum {
    MIC_ASR_DOUBAO_VAD_WAITING_FOR_SPEECH = 0,                     // 尚未检测到有效语音。
    MIC_ASR_DOUBAO_VAD_IN_SPEECH,                                  // 已检测到说话，正在发送语音流。
    MIC_ASR_DOUBAO_VAD_ENDING,                                     // 已决定结束，本地不再接受后续 PCM。
} mic_asr_doubao_vad_state_t;

/**
 * @brief 豆包 ASR 流式会话上下文。
 *
 * 调用方法：本文件内部使用一个静态上下文维护当前 ASR 会话。manual_ws_client_t
 * 只承担 WebSocket 协议连接状态，其他字段都属于豆包 ASR 业务层。
 */
typedef struct {
    manual_ws_client_t ws;                                         // 通用 WebSocket/TLS 连接。
    bool session_ready;                                            // true 表示 WebSocket 握手和 full client request 已完成。
    bool response_done;                                            // true 表示已得到最终结果或服务端 close。
    bool response_error;                                           // true 表示服务端返回错误或帧/协议异常。
    bool connection_broken;                                        // true 表示发送/接收已失败；后续不再发送 last 包或 close frame。

    char connect_id[MIC_ASR_DOUBAO_CONNECT_ID_MAX_LEN];            // 本次握手的 X-Api-Connect-Id。
    char headers[MIC_ASR_DOUBAO_HEADERS_MAX_LEN];                  // 只保存豆包 X-Api-* 业务 Header。
    char recognized_text[MIC_ASR_DOUBAO_RESULT_TEXT_MAX_LEN];      // 最新识别文本。
    char interim_text[MIC_ASR_DOUBAO_RESULT_TEXT_MAX_LEN];         // 最近一次已打印的 interim 文本，用于变化去重。
    mic_asr_doubao_final_key_t final_history[MIC_ASR_DOUBAO_FINAL_DEDUP_HISTORY]; // 最近 final 输出历史。
    size_t final_history_next;                                     // final_history 的环形写入位置。

    uint8_t frame_buffer[MIC_ASR_DOUBAO_FRAME_BUFFER_BYTES];       // 12 字节音频协议前缀 + PCM payload；JSON 首包复用前 8 字节前缀。
    size_t pending_audio_bytes;                                    // frame_buffer 中保留的“最后一包候选”PCM 字节数。
    size_t sent_audio_bytes;                                       // 已发送给 ASR 的 PCM 字节数，仅用于日志。
    uint32_t sent_audio_packet_count;                              // 已成功发送的 PCM 音频业务包序号，从 1 开始递增。
    uint32_t pcm_quality_packet_count;                             // 本会话已经统计过的实际 PCM 音频包数量，不包含 0 字节 final 包。
    uint32_t pcm_quality_silence_streak;                           // 连续 p2p 很小的 PCM 包数量，用于判断长期接近静音。
    mic_asr_doubao_vad_state_t vad_state;                          // 本地 RMS 端点检测状态。
    bool local_audio_finished;                                      // true 表示已经发送 LAST_AUDIO_ONLY_REQUEST，后续不再发送 PCM。
    uint32_t vad_audio_ms;                                         // 已发送/准备发送的本地音频时长，按 100 ms 包累计。
    uint32_t vad_speech_ms;                                        // 从检测到说话开始累计的时长。
    uint32_t vad_silence_ms;                                       // IN_SPEECH 后连续低 RMS 静音时长。

    uint8_t rx_frame_buffer[MANUAL_WS_MAX_PAYLOAD_SIZE];           // manual_ws_recv_frame() 的单帧 payload 缓冲。
    uint8_t *rx_message_buffer;                                    // WebSocket continuation 业务消息重组缓冲。
    size_t rx_message_len;                                         // 当前已累计的业务消息长度。
    bool rx_message_active;                                        // true 表示正在重组一个分片 binary message。
    uint32_t rx_ws_frame_count;                                    // 本会话已收到的服务端 WebSocket frame 数量，用于协议排查日志定位顺序。
} mic_asr_doubao_session_t;

static mic_asr_doubao_session_t s_asr;

static esp_err_t mic_asr_doubao_poll_server_once(int timeout_ms);

/**
 * @brief ASR final 文本占位回调的默认空实现。
 *
 * 调用方法：收到去重后的 definite=true utterance 时调用。这里故意不接云端 LLM，
 * 后续业务模块如果需要消费 final ASR 文本，可定义同名强符号函数覆盖本默认实现。
 */
__attribute__((weak)) void mic_asr_on_final_text(const char *text)
{
    (void)text;
}

/**
 * @brief 打印当前堆内存状态。
 *
 * 调用方法：ASR start、manual_ws 建连前后和 stop 后调用，用于观察 TLS/WebSocket
 * 拆分后堆内存是否稳定。
 */
static void mic_asr_doubao_log_heap(const char *label)
{
#if MIC_ASR_DOUBAO_ENABLE_START_DEBUG_LOG
    ESP_LOGI(TAG,
             "%s heap: free=%" PRIu32 ", minimum=%" PRIu32 ", largest_8bit=%u",
             label,
             esp_get_free_heap_size(),
             esp_get_minimum_free_heap_size(),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
#else
    (void)label;
#endif
}

/**
 * @brief 生成 RFC 4122 v4 形态的 UUID 字符串。
 *
 * 调用方法：每次 ASR WebSocket 建连前调用，用作 X-Api-Connect-Id。输出格式固定为
 * xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx。
 */
static void mic_asr_doubao_make_uuid(char *out, size_t out_len)
{
    if (out == NULL || out_len < 37) {
        if (out != NULL && out_len > 0) {
            out[0] = '\0';
        }
        return;
    }

    uint8_t uuid[16] = {0};
    for (size_t i = 0; i < sizeof(uuid); i += sizeof(uint32_t)) {
        uint32_t random_value = esp_random();
        uuid[i] = (uint8_t)(random_value & 0xFFU);
        uuid[i + 1] = (uint8_t)((random_value >> 8) & 0xFFU);
        uuid[i + 2] = (uint8_t)((random_value >> 16) & 0xFFU);
        uuid[i + 3] = (uint8_t)((random_value >> 24) & 0xFFU);
    }

    uuid[6] = (uint8_t)((uuid[6] & 0x0FU) | 0x40U);
    uuid[8] = (uint8_t)((uuid[8] & 0x3FU) | 0x80U);

    snprintf(out,
             out_len,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             (unsigned int)uuid[0],
             (unsigned int)uuid[1],
             (unsigned int)uuid[2],
             (unsigned int)uuid[3],
             (unsigned int)uuid[4],
             (unsigned int)uuid[5],
             (unsigned int)uuid[6],
             (unsigned int)uuid[7],
             (unsigned int)uuid[8],
             (unsigned int)uuid[9],
             (unsigned int)uuid[10],
             (unsigned int)uuid[11],
             (unsigned int)uuid[12],
             (unsigned int)uuid[13],
             (unsigned int)uuid[14],
             (unsigned int)uuid[15]);
}

/**
 * @brief 选择本次握手使用的 X-Api-Connect-Id。
 *
 * 调用方法：start() 组装豆包业务 Header 前调用。配置为空时自动生成 UUID；
 * 配置非空时使用配置值，便于临时和服务端日志对齐。
 */
static void mic_asr_doubao_prepare_connect_id(void)
{
    if (MIC_ASR_DOUBAO_CONNECT_ID[0] == '\0') {
        mic_asr_doubao_make_uuid(s_asr.connect_id, sizeof(s_asr.connect_id));
        return;
    }

    strlcpy(s_asr.connect_id, MIC_ASR_DOUBAO_CONNECT_ID, sizeof(s_asr.connect_id));
}

#if MIC_ASR_DOUBAO_ENABLE_START_DEBUG_LOG
/**
 * @brief 生成“前三后三”形式的密钥脱敏摘要。
 *
 * 调用方法：打印配置摘要时调用，避免 App Key / Access Key 明文出现在串口日志。
 */
static void mic_asr_doubao_make_masked_key_summary(const char *secret, char *out, size_t out_len)
{
    if (secret == NULL || out == NULL || out_len == 0) {
        return;
    }

    size_t secret_len = strlen(secret);
    if (secret_len < 7) {
        strlcpy(out, "***", out_len);
        return;
    }

    snprintf(out, out_len, "%.3s***%s", secret, secret + secret_len - 3);
}
#endif

/**
 * @brief 打印豆包 ASR WebSocket 业务配置摘要。
 *
 * 调用方法：manual_ws_connect() 前调用。这里只打印 URI/Resource ID/Connect ID 和
 * 密钥脱敏摘要；manual_ws 层不会看到这些字段的业务含义。
 */
static void mic_asr_doubao_log_config(void)
{
#if MIC_ASR_DOUBAO_ENABLE_START_DEBUG_LOG
    char masked_app_key[16] = {0};
    char masked_access_key[16] = {0};
    mic_asr_doubao_make_masked_key_summary(MIC_ASR_DOUBAO_APP_KEY,
                                           masked_app_key,
                                           sizeof(masked_app_key));
    mic_asr_doubao_make_masked_key_summary(MIC_ASR_DOUBAO_ACCESS_KEY,
                                           masked_access_key,
                                           sizeof(masked_access_key));

    ESP_LOGI(TAG, "ASR URI: %s", MIC_ASR_DOUBAO_WS_URI);
    ESP_LOGI(TAG, "ASR host=%s path=%s port=%d",
             MIC_ASR_DOUBAO_WS_HOST,
             MIC_ASR_DOUBAO_WS_PATH,
             MIC_ASR_DOUBAO_WS_PORT);
    ESP_LOGI(TAG, "ASR Resource ID: %s", MIC_ASR_DOUBAO_RESOURCE_ID);
    ESP_LOGI(TAG,
             "ASR App Key length=%u masked=%s",
             (unsigned int)strlen(MIC_ASR_DOUBAO_APP_KEY),
             masked_app_key);
    ESP_LOGI(TAG,
             "ASR Access Key length=%u masked=%s",
             (unsigned int)strlen(MIC_ASR_DOUBAO_ACCESS_KEY),
             masked_access_key);
    ESP_LOGI(TAG, "ASR Connect ID: %s", s_asr.connect_id);
#endif
}

/**
 * @brief 组装豆包 ASR 鉴权 Header。
 *
 * 调用方法：start() 在 manual_ws_connect() 前调用。这里故意只放豆包业务要求的
 * X-Api-App-Key、X-Api-Access-Key、X-Api-Resource-Id、X-Api-Connect-Id；
 * Host、Connection、Upgrade、Sec-WebSocket-Key、Sec-WebSocket-Version 由 manual_ws 生成。
 */
static esp_err_t mic_asr_doubao_build_headers(void)
{
    int written = snprintf(s_asr.headers,
                           sizeof(s_asr.headers),
                           "X-Api-App-Key: %s\r\n"
                           "X-Api-Access-Key: %s\r\n"
                           "X-Api-Resource-Id: %s\r\n"
                           "X-Api-Connect-Id: %s\r\n",
                           MIC_ASR_DOUBAO_APP_KEY,
                           MIC_ASR_DOUBAO_ACCESS_KEY,
                           MIC_ASR_DOUBAO_RESOURCE_ID,
                           s_asr.connect_id);
    if (written < 0 || (size_t)written >= sizeof(s_asr.headers)) {
        ESP_LOGE(TAG, "ASR Header 组装失败: buffer too small");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/**
 * @brief 释放 WebSocket continuation 业务消息重组缓冲。
 *
 * 调用方法：开始新会话、解析完成、收到异常分片或 stop 时调用。
 */
static void mic_asr_doubao_free_rx_message_buffer(void)
{
    if (s_asr.rx_message_buffer != NULL) {
        free(s_asr.rx_message_buffer);
        s_asr.rx_message_buffer = NULL;
    }
    s_asr.rx_message_len = 0;
    s_asr.rx_message_active = false;
}

/**
 * @brief 标记当前 ASR 连接已经不可继续收发。
 *
 * 调用方法：manual_ws_send_binary()/manual_ws_recv_frame() 返回真实错误、收到 SERVER_ERROR、
 * 解析到不可恢复协议错误时调用。标记后：
 * - mic_asr_doubao_send_pcm() 不再继续发送后续 PCM；
 * - mic_asr_doubao_finish() 不再发送 LAST_AUDIO_ONLY_REQUEST；
 * - mic_asr_doubao_stop() 会先把 ws.connected 置 false，再销毁 TLS，避免 broken 连接上
 *   继续发送 WebSocket close frame。
 */
static void mic_asr_doubao_mark_connection_broken(const char *reason)
{
    if (!s_asr.connection_broken) {
        ESP_LOGE(TAG, "ASR connection broken: %s", reason != NULL ? reason : "unknown");
    }
    s_asr.connection_broken = true;
    s_asr.response_error = true;
    s_asr.response_done = true;
}

/**
 * @brief 清空不需要跨会话保留的状态。
 *
 * 调用方法：start 前和 stop 后调用。ws 连接必须先由 manual_ws_close() 释放。
 */
static void mic_asr_doubao_reset_session_state(void)
{
    mic_asr_doubao_free_rx_message_buffer();
    memset(&s_asr.ws, 0, sizeof(s_asr.ws));
    s_asr.session_ready = false;
    s_asr.response_done = false;
    s_asr.response_error = false;
    s_asr.connection_broken = false;
    s_asr.pending_audio_bytes = 0;
    s_asr.sent_audio_bytes = 0;
    s_asr.sent_audio_packet_count = 0;
    s_asr.pcm_quality_packet_count = 0;
    s_asr.pcm_quality_silence_streak = 0;
    s_asr.vad_state = MIC_ASR_DOUBAO_VAD_WAITING_FOR_SPEECH;
    s_asr.local_audio_finished = false;
    s_asr.vad_audio_ms = 0;
    s_asr.vad_speech_ms = 0;
    s_asr.vad_silence_ms = 0;
    s_asr.rx_ws_frame_count = 0;
    s_asr.final_history_next = 0;
    s_asr.connect_id[0] = '\0';
    s_asr.headers[0] = '\0';
    s_asr.recognized_text[0] = '\0';
    s_asr.interim_text[0] = '\0';
    memset(s_asr.final_history, 0, sizeof(s_asr.final_history));
}

/**
 * @brief 把 32 位长度写成大端字节序。
 *
 * 调用方法：发送豆包二进制协议帧时，把 payload size 写入协议前缀后 4 字节。
 */
static void mic_asr_doubao_u32_to_be_bytes(uint32_t value,
                                           uint8_t out_bytes[MIC_ASR_DOUBAO_PROTOCOL_LENGTH_BYTES])
{
    out_bytes[0] = (uint8_t)((value >> 24) & 0xFFU);
    out_bytes[1] = (uint8_t)((value >> 16) & 0xFFU);
    out_bytes[2] = (uint8_t)((value >> 8) & 0xFFU);
    out_bytes[3] = (uint8_t)(value & 0xFFU);
}

/**
 * @brief 把 32 位有符号 sequence 写成大端字节序。
 *
 * 调用方法：发送 AUDIO_ONLY_REQUEST 和 LAST_AUDIO_ONLY_REQUEST 时调用。
 * FULL_CLIENT_REQUEST 对应的服务端首个响应已经占用 sequence=1，因此普通音频包从
 * MIC_ASR_DOUBAO_FIRST_AUDIO_SEQUENCE 开始递增；最后一包传入对应负数。
 */
static void mic_asr_doubao_i32_to_be_bytes(int32_t value,
                                           uint8_t out_bytes[MIC_ASR_DOUBAO_PROTOCOL_SEQUENCE_BYTES])
{
    uint32_t raw = (uint32_t)value;
    out_bytes[0] = (uint8_t)((raw >> 24) & 0xFFU);
    out_bytes[1] = (uint8_t)((raw >> 16) & 0xFFU);
    out_bytes[2] = (uint8_t)((raw >> 8) & 0xFFU);
    out_bytes[3] = (uint8_t)(raw & 0xFFU);
}

/**
 * @brief 从大端字节序读取 32 位长度。
 *
 * 调用方法：解析服务端豆包二进制协议帧时读取 payload size。
 */
static uint32_t mic_asr_doubao_u32_from_be_bytes(const uint8_t bytes[MIC_ASR_DOUBAO_PROTOCOL_LENGTH_BYTES])
{
    return ((uint32_t)bytes[0] << 24) |
           ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) |
           (uint32_t)bytes[3];
}

/**
 * @brief 从大端字节序读取有符号 32 位 sequence。
 *
 * 调用方法：解析豆包服务端 ACK/错误响应或打印本地发送包协议头时调用。豆包协议把
 * sequence 当作 4 字节有符号整数传输，这里先按 uint32_t 拼出原始位模式，再转换成
 * int32_t，方便把负序号 last 包直接打出来。
 */
static int32_t mic_asr_doubao_i32_from_be_bytes(const uint8_t bytes[MIC_ASR_DOUBAO_PROTOCOL_LENGTH_BYTES])
{
    uint32_t raw = mic_asr_doubao_u32_from_be_bytes(bytes);
    return (int32_t)raw;
}

/**
 * @brief 把豆包 message type 转成人类可读名称。
 *
 * 调用方法：发送/接收协议诊断日志统一调用，避免只看到 0x2/0x9/0xB 时还要翻协议表。
 */
#if MIC_ASR_DOUBAO_ENABLE_PROTOCOL_DEBUG_LOG
static const char *mic_asr_doubao_message_type_name(uint8_t message_type)
{
    switch (message_type) {
    case 0x1:
        return "FULL_CLIENT_REQUEST";
    case 0x2:
        return "AUDIO_ONLY_REQUEST";
    case MIC_ASR_DOUBAO_PARSER_MSG_FULL_RESPONSE:
        return "FULL_SERVER_RESPONSE";
    case MIC_ASR_DOUBAO_PARSER_MSG_SERVER_ACK:
        return "SERVER_ACK";
    case MIC_ASR_DOUBAO_PARSER_MSG_SERVER_ERROR:
        return "SERVER_ERROR";
    default:
        return "UNKNOWN";
    }
}
#endif

/**
 * @brief 把豆包 serialization 转成人类可读名称。
 *
 * 调用方法：协议诊断日志打印第 3 字节高 4 bit 时调用。客户端请求期望 JSON，
 * 音频包通常是 raw/none。
 */
#if MIC_ASR_DOUBAO_ENABLE_PROTOCOL_DEBUG_LOG
static const char *mic_asr_doubao_serialization_name(uint8_t serialization)
{
    switch (serialization) {
    case MIC_ASR_DOUBAO_PARSER_SERIAL_NONE:
        return "none";
    case MIC_ASR_DOUBAO_PARSER_SERIAL_JSON:
        return "json";
    case MIC_ASR_DOUBAO_PARSER_SERIAL_PROTOBUF:
        return "protobuf";
    default:
        return "unknown";
    }
}
#endif

/**
 * @brief 把豆包 compression 转成人类可读名称。
 *
 * 调用方法：协议诊断日志打印第 3 字节低 4 bit 时调用。当前固件只识别 gzip，
 * 不在发送端启用压缩。
 */
#if MIC_ASR_DOUBAO_ENABLE_PROTOCOL_DEBUG_LOG
static const char *mic_asr_doubao_compression_name(uint8_t compression)
{
    switch (compression) {
    case MIC_ASR_DOUBAO_PARSER_COMPRESS_NONE:
        return "none";
    case MIC_ASR_DOUBAO_PARSER_COMPRESS_GZIP:
        return "gzip";
    default:
        return "unknown";
    }
}
#endif

/**
 * @brief 打印任意二进制数据前 N 字节 hex。
 *
 * 调用方法：服务端 payload 诊断只打印前 MIC_ASR_DOUBAO_SERVER_PAYLOAD_HEX_PREVIEW_BYTES
 * 字节；发送端只打印豆包 4 字节协议头，不打印 PCM 内容。函数使用固定栈缓冲，
 * 不保存整句音频，也不会把 payload 当字符串直接输出。
 */
#if MIC_ASR_DOUBAO_ENABLE_PROTOCOL_DEBUG_LOG
static void mic_asr_doubao_log_hex_preview(const char *label,
                                           const uint8_t *data,
                                           size_t data_len,
                                           size_t max_preview_len)
{
    if (label == NULL) {
        return;
    }
    if (data == NULL || data_len == 0 || max_preview_len == 0) {
        ESP_LOGI(TAG, "%s hex=<empty>", label);
        return;
    }

    size_t preview_len = data_len;
    if (preview_len > max_preview_len) {
        preview_len = max_preview_len;
    }

    char hex[MIC_ASR_DOUBAO_SERVER_PAYLOAD_HEX_PREVIEW_BYTES * 3 + 1] = {0};
    size_t used = 0;
    for (size_t i = 0; i < preview_len && used + 3 < sizeof(hex); i++) {
        int written = snprintf(&hex[used],
                               sizeof(hex) - used,
                               "%02X%s",
                               (unsigned int)data[i],
                               (i + 1 < preview_len) ? " " : "");
        if (written <= 0) {
            break;
        }
        used += (size_t)written;
    }

    ESP_LOGI(TAG,
             "%s hex(%u/%u)=%s%s",
             label,
             (unsigned int)preview_len,
             (unsigned int)data_len,
             hex,
             data_len > preview_len ? " ..." : "");
}
#endif

/**
 * @brief 打印豆包业务协议头字段。
 *
 * 调用方法：发送 full/audio/final 前和接收服务端 binary payload 后调用。日志包含
 * version、header size、message type、flags、serialization、compression、sequence
 * 和 payload size，用来确认 final 包确实是 last audio packet，而不是普通 0 字节
 * WebSocket 包。
 */
#if MIC_ASR_DOUBAO_ENABLE_PROTOCOL_DEBUG_LOG
static void mic_asr_doubao_log_protocol_header(const char *direction,
                                               const uint8_t *data,
                                               size_t len)
{
    if (direction == NULL) {
        direction = "ASR";
    }
    if (data == NULL || len < MIC_ASR_DOUBAO_FULL_REQUEST_PREFIX_BYTES) {
        ESP_LOGW(TAG,
                 "%s protocol header invalid: data=%p len=%u",
                 direction,
                 data,
                 (unsigned int)len);
        return;
    }

    uint8_t version =
        (data[0] & MIC_ASR_DOUBAO_PARSER_VERSION_MASK) >> MIC_ASR_DOUBAO_PARSER_VERSION_SHIFT;
    uint8_t header_words = data[0] & MIC_ASR_DOUBAO_PARSER_HEADER_SIZE_MASK;
    size_t header_bytes = (size_t)header_words * MIC_ASR_DOUBAO_PARSER_HEADER_WORD_BYTES;
    uint8_t message_type =
        (data[1] & MIC_ASR_DOUBAO_PARSER_MSG_TYPE_MASK) >> MIC_ASR_DOUBAO_PARSER_MSG_TYPE_SHIFT;
    uint8_t flags = data[1] & MIC_ASR_DOUBAO_PARSER_FLAGS_MASK;
    uint8_t serialization =
        (data[2] & MIC_ASR_DOUBAO_PARSER_SERIAL_MASK) >> MIC_ASR_DOUBAO_PARSER_SERIAL_SHIFT;
    uint8_t compression = data[2] & MIC_ASR_DOUBAO_PARSER_COMPRESS_MASK;

    if (header_bytes < MIC_ASR_DOUBAO_PARSER_HEADER_BYTES || header_bytes > len) {
        ESP_LOGW(TAG,
                 "%s protocol header size invalid: version=%u header_words=%u header_bytes=%u len=%u",
                 direction,
                 (unsigned int)version,
                 (unsigned int)header_words,
                 (unsigned int)header_bytes,
                 (unsigned int)len);
        return;
    }

    size_t cursor = header_bytes;
    bool has_sequence = flags == MIC_ASR_DOUBAO_PARSER_FLAG_POS_SEQUENCE ||
                        flags == MIC_ASR_DOUBAO_PARSER_FLAG_NEG_SEQUENCE;
    int32_t sequence = 0;
    if (has_sequence) {
        if (len - cursor < MIC_ASR_DOUBAO_PARSER_SEQUENCE_BYTES +
                           MIC_ASR_DOUBAO_PARSER_PAYLOAD_SIZE_BYTES) {
            ESP_LOGW(TAG,
                     "%s protocol sequence/payload-size truncated: len=%u header_bytes=%u",
                     direction,
                     (unsigned int)len,
                     (unsigned int)header_bytes);
            return;
        }
        sequence = mic_asr_doubao_i32_from_be_bytes(&data[cursor]);
        cursor += MIC_ASR_DOUBAO_PARSER_SEQUENCE_BYTES;
    }

    if (len - cursor < MIC_ASR_DOUBAO_PARSER_PAYLOAD_SIZE_BYTES) {
        ESP_LOGW(TAG,
                 "%s protocol payload-size truncated: len=%u cursor=%u",
                 direction,
                 (unsigned int)len,
                 (unsigned int)cursor);
        return;
    }

    uint32_t payload_size = mic_asr_doubao_u32_from_be_bytes(&data[cursor]);
    ESP_LOGI(TAG,
             "%s protocol: total_len=%u header_bytes=%u raw_header=%02X %02X %02X %02X version=%u message_type=0x%X(%s) flags=0x%X sequence=%s%d serialization=0x%X(%s) compression=0x%X(%s) payload_size=%" PRIu32,
             direction,
             (unsigned int)len,
             (unsigned int)header_bytes,
             (unsigned int)data[0],
             (unsigned int)data[1],
             (unsigned int)data[2],
             (unsigned int)data[3],
             (unsigned int)version,
             (unsigned int)message_type,
             mic_asr_doubao_message_type_name(message_type),
             (unsigned int)flags,
             has_sequence ? "" : "none/",
             has_sequence ? (int)sequence : 0,
             (unsigned int)serialization,
             mic_asr_doubao_serialization_name(serialization),
             (unsigned int)compression,
             mic_asr_doubao_compression_name(compression),
             payload_size);
}
#endif

/**
 * @brief 对 64 位无符号整数做整数平方根。
 *
 * 调用方法：PCM 质量统计计算 RMS 时调用。这里不用浮点 sqrt，避免为了调试日志
 * 给固件额外引入浮点数学库依赖。
 *
 * @param value 要开平方的无符号整数。
 * @return floor(sqrt(value))。
 */
static uint32_t mic_asr_doubao_isqrt_u64(uint64_t value)
{
    uint64_t result = 0;
    uint64_t bit = (uint64_t)1 << 62;

    while (bit > value) {
        bit >>= 2;
    }

    while (bit != 0) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }

    return (uint32_t)result;
}

/**
 * @brief 计算一个 PCM_s16le 包的 RMS。
 *
 * 调用方法：每次发送 100 ms AUDIO_ONLY_REQUEST 前调用，用于本地端点检测。函数只读
 * 当前待发送 PCM，不改变 PCM 内容、不参与豆包协议封包；payload 必须是 signed int16
 * little-endian PCM。
 *
 * @param pcm_payload 当前 100 ms PCM 包。
 * @param payload_bytes PCM 字节数，必须是 int16_t 的整数倍。
 * @return 当前包 pcm_rms；参数非法时返回 0。
 */
static uint32_t mic_asr_doubao_calc_pcm_rms(const int16_t *pcm_payload, size_t payload_bytes)
{
    if (pcm_payload == NULL || payload_bytes == 0 || (payload_bytes % sizeof(int16_t)) != 0) {
        return 0;
    }

    size_t sample_count = payload_bytes / sizeof(int16_t);
    uint64_t sum_square_samples = 0;
    for (size_t i = 0; i < sample_count; i++) {
        int32_t sample = pcm_payload[i];
        sum_square_samples += (uint64_t)(sample * sample);
    }

    return mic_asr_doubao_isqrt_u64(sum_square_samples / sample_count);
}

/**
 * @brief 判断当前 PCM 包是否需要打印质量统计。
 *
 * 调用方法：mic_asr_doubao_log_pcm_packet_quality() 每处理一个实际发送的非 0 字节
 * PCM payload 后调用。每 MIC_ASR_DOUBAO_PCM_QUALITY_LOG_INTERVAL_PACKETS 包打印一次，
 * 也就是默认每 20 包打印 pcm_min/pcm_max/pcm_avg/pcm_rms/zero_cross。
 *
 * @param packet_index 当前实际 PCM 包序号，从 1 开始，不包含 0 字节 final 包。
 * @return 需要打印返回 true，否则返回 false。
 */
#if MIC_ASR_DOUBAO_ENABLE_PCM_QUALITY_LOG
static bool mic_asr_doubao_should_log_pcm_quality(uint32_t packet_index)
{
#if MIC_ASR_DOUBAO_PCM_QUALITY_LOG_EVERY_PACKET
    (void)packet_index;
    return true;
#else
    return (packet_index % MIC_ASR_DOUBAO_PCM_QUALITY_LOG_INTERVAL_PACKETS) == 0;
#endif
}
#endif

/**
 * @brief 扫描一次即将发送的 PCM payload 并打印质量统计。
 *
 * 调用方法：mic_asr_doubao_flush_pending_audio() 在发送非 0 字节 PCM payload 前调用。
 * 本函数只读取 frame_buffer 中当前即将发送的 payload，按 int16_t little-endian
 * PCM_s16le 统计 pcm_min、pcm_max、pcm_avg、pcm_rms 和 zero_cross，不复制、不暂存、
 * 不打印任何 PCM 原始样本，也不改变后续发送协议。
 *
 * 由于当前 ESP32-C5 平台按 little-endian 访问 int16_t，这里直接读取 int16_t 样本；
 * 如果后续移植到非 little-endian 平台，应改为逐字节拼出 int16_t。
 *
 * @param pcm_payload 当前即将发送的 PCM payload 指针，不能为空。
 * @param payload_bytes 当前即将发送的 PCM payload 字节数，必须是 int16_t 的整数倍。
 * @param packet_index 当前实际 PCM 包序号，从 1 开始，不包含 0 字节 final 包。
 */
static void mic_asr_doubao_log_pcm_packet_quality(const int16_t *pcm_payload,
                                                  size_t payload_bytes,
                                                  uint32_t packet_index)
{
#if MIC_ASR_DOUBAO_ENABLE_PCM_QUALITY_LOG
    if (pcm_payload == NULL || payload_bytes == 0 || (payload_bytes % sizeof(int16_t)) != 0) {
        return;
    }

    size_t sample_count = payload_bytes / sizeof(int16_t);
    int16_t min_sample = INT16_MAX;
    int16_t max_sample = INT16_MIN;
    int64_t sum_samples = 0;
    uint64_t sum_square_samples = 0;
    size_t clip_like_count = 0;
    uint32_t zero_cross = 0;
    int16_t previous_sample = 0;
    bool has_previous_sample = false;

    for (size_t i = 0; i < sample_count; i++) {
        int16_t sample = pcm_payload[i];
        int32_t sample_i32 = sample;

        if (sample < min_sample) {
            min_sample = sample;
        }
        if (sample > max_sample) {
            max_sample = sample;
        }
        if (sample <= MIC_ASR_DOUBAO_PCM_CLIP_NEAR_MIN ||
            sample >= MIC_ASR_DOUBAO_PCM_CLIP_NEAR_MAX) {
            clip_like_count++;
        }

        if (has_previous_sample &&
            ((previous_sample < 0 && sample >= 0) ||
             (previous_sample >= 0 && sample < 0))) {
            zero_cross++;
        }
        previous_sample = sample;
        has_previous_sample = true;

        sum_samples += sample_i32;
        sum_square_samples += (uint64_t)(sample_i32 * sample_i32);
    }

    int32_t avg_sample = (int32_t)(sum_samples / (int64_t)sample_count);
    uint32_t rms_sample = mic_asr_doubao_isqrt_u64(sum_square_samples / sample_count);
    uint32_t p2p_sample = (uint32_t)((int32_t)max_sample - (int32_t)min_sample);
    uint32_t clip_percent = (uint32_t)((clip_like_count * 100U) / sample_count);

    if (p2p_sample <= MIC_ASR_DOUBAO_PCM_SILENCE_P2P_THRESHOLD) {
        s_asr.pcm_quality_silence_streak++;
    } else {
        s_asr.pcm_quality_silence_streak = 0;
    }

    if (mic_asr_doubao_should_log_pcm_quality(packet_index)) {
        ESP_LOGI(TAG,
                 "ASR PCM quality packet=%" PRIu32 ", payload_bytes=%u, pcm_min=%d, pcm_max=%d, pcm_avg=%" PRId32 ", pcm_rms=%" PRIu32 ", zero_cross=%" PRIu32 ", p2p=%" PRIu32 ", clip_like=%u%%",
                 packet_index,
                 (unsigned int)payload_bytes,
                 (int)min_sample,
                 (int)max_sample,
                 avg_sample,
                 rms_sample,
                 zero_cross,
                 p2p_sample,
                 (unsigned int)clip_percent);
    }

    if (mic_asr_doubao_should_log_pcm_quality(packet_index) &&
        s_asr.pcm_quality_silence_streak >= MIC_ASR_DOUBAO_PCM_SILENCE_WARN_CONSECUTIVE) {
        ESP_LOGW(TAG,
                 "PCM 可能接近静音: consecutive=%" PRIu32 ", p2p=%" PRIu32 ", rms=%" PRIu32,
                 s_asr.pcm_quality_silence_streak,
                 p2p_sample,
                 rms_sample);
    }

    if (mic_asr_doubao_should_log_pcm_quality(packet_index) &&
        clip_percent >= MIC_ASR_DOUBAO_PCM_CLIP_WARN_PERCENT) {
        ESP_LOGW(TAG,
                 "PCM 可能削波: min=%d, max=%d, clip_like=%u%%",
                 (int)min_sample,
                 (int)max_sample,
                 (unsigned int)clip_percent);
    }
#else
    (void)pcm_payload;
    (void)payload_bytes;
    (void)packet_index;
#endif
}

/**
 * @brief 用当前 100 ms PCM 包推进本地 RMS VAD 状态机。
 *
 * 调用方法：mic_asr_doubao_flush_pending_audio() 在真正发送当前包前调用。返回 true
 * 表示当前包应作为 LAST_AUDIO_ONLY_REQUEST 发送，随后本地音频发送循环停止；返回 false
 * 表示继续按普通 AUDIO_ONLY_REQUEST 发送。状态机只依赖当前包 pcm_rms 和已累计包时长：
 * WAITING_FOR_SPEECH -> IN_SPEECH -> ENDING。
 *
 * @param packet_index 当前将要发送的音频业务包序号。
 * @param sequence_abs 当前包对应的正 sequence；若结束，将取负数发送。
 * @param pcm_rms 当前包 RMS。
 * @return 当前包是否应作为 LAST_AUDIO_ONLY_REQUEST 发送。
 */
static bool mic_asr_doubao_update_local_vad(uint32_t packet_index,
                                            int32_t sequence_abs,
                                            uint32_t pcm_rms)
{
    if (s_asr.local_audio_finished || s_asr.vad_state == MIC_ASR_DOUBAO_VAD_ENDING) {
        return true;
    }

    s_asr.vad_audio_ms += MIC_ASR_DOUBAO_PACKET_MS;

    if (s_asr.vad_audio_ms >= MIC_ASR_DOUBAO_VAD_MAX_RECORD_MS) {
        s_asr.vad_state = MIC_ASR_DOUBAO_VAD_ENDING;
        ESP_LOGI(TAG,
                 "ASR VAD: max_record_ms reached, sending LAST_AUDIO_ONLY_REQUEST packet=%" PRIu32 ", sequence=%" PRId32,
                 packet_index,
                 -sequence_abs);
        return true;
    }

    switch (s_asr.vad_state) {
    case MIC_ASR_DOUBAO_VAD_WAITING_FOR_SPEECH:
        if (pcm_rms >= MIC_ASR_DOUBAO_VAD_SPEECH_START_RMS) {
            s_asr.vad_state = MIC_ASR_DOUBAO_VAD_IN_SPEECH;
            s_asr.vad_speech_ms = MIC_ASR_DOUBAO_PACKET_MS;
            s_asr.vad_silence_ms = 0;
            ESP_LOGI(TAG,
                     "ASR VAD: speech started at packet=%" PRIu32 ", rms=%" PRIu32,
                     packet_index,
                     pcm_rms);
        }
        break;

    case MIC_ASR_DOUBAO_VAD_IN_SPEECH:
        s_asr.vad_speech_ms += MIC_ASR_DOUBAO_PACKET_MS;
        if (pcm_rms < MIC_ASR_DOUBAO_VAD_SILENCE_END_RMS) {
            s_asr.vad_silence_ms += MIC_ASR_DOUBAO_PACKET_MS;
            ESP_LOGI(TAG,
                     "ASR VAD: silence_ms=%" PRIu32 ", rms=%" PRIu32,
                     s_asr.vad_silence_ms,
                     pcm_rms);
        } else {
            s_asr.vad_silence_ms = 0;
        }

        if (s_asr.vad_speech_ms >= MIC_ASR_DOUBAO_VAD_MIN_RECORD_MS &&
            s_asr.vad_silence_ms >= MIC_ASR_DOUBAO_VAD_SILENCE_END_MS) {
            s_asr.vad_state = MIC_ASR_DOUBAO_VAD_ENDING;
            ESP_LOGI(TAG,
                     "ASR VAD: speech ended, sending LAST_AUDIO_ONLY_REQUEST packet=%" PRIu32 ", sequence=%" PRId32,
                     packet_index,
                     -sequence_abs);
            return true;
        }
        break;

    case MIC_ASR_DOUBAO_VAD_ENDING:
        return true;

    default:
        s_asr.vad_state = MIC_ASR_DOUBAO_VAD_WAITING_FOR_SPEECH;
        break;
    }

    return false;
}

/**
 * @brief 打印即将发送的音频包协议字段和 PCM 前缀。
 *
 * 调用方法：每次发送 AUDIO_ONLY_REQUEST / LAST_AUDIO_ONLY_REQUEST 前调用。
 * 日志只打印 sequence、flags、payload_size 和前 MIC_ASR_DOUBAO_PCM_SEND_HEX_PREVIEW_BYTES
 * 个 PCM 字节，不打印整包音频；用于确认发送端协议头和音频字节序。
 */
static void mic_asr_doubao_log_audio_send_debug(const int16_t *pcm_payload,
                                                size_t payload_bytes,
                                                int32_t sequence,
                                                uint8_t flags)
{
#if MIC_ASR_DOUBAO_ENABLE_PROTOCOL_DEBUG_LOG
    if (pcm_payload == NULL || payload_bytes == 0) {
        ESP_LOGI(TAG,
                 "ASR AUDIO_ONLY debug: sequence=%" PRId32 ", flags=0x%X, payload_size=%u, pcm_first_hex=<empty>",
                 sequence,
                 (unsigned int)flags,
                 (unsigned int)payload_bytes);
        return;
    }

    size_t preview_len = payload_bytes;
    if (preview_len > MIC_ASR_DOUBAO_PCM_SEND_HEX_PREVIEW_BYTES) {
        preview_len = MIC_ASR_DOUBAO_PCM_SEND_HEX_PREVIEW_BYTES;
    }

    char hex[MIC_ASR_DOUBAO_PCM_SEND_HEX_PREVIEW_BYTES * 3 + 1] = {0};
    size_t used = 0;
    const uint8_t *pcm_bytes = (const uint8_t *)pcm_payload;
    for (size_t i = 0; i < preview_len && used + 3 < sizeof(hex); i++) {
        int written = snprintf(&hex[used],
                               sizeof(hex) - used,
                               "%02X%s",
                               (unsigned int)pcm_bytes[i],
                               (i + 1 < preview_len) ? " " : "");
        if (written <= 0) {
            break;
        }
        used += (size_t)written;
    }

    ESP_LOGI(TAG,
             "ASR AUDIO_ONLY debug: sequence=%" PRId32 ", flags=0x%X, payload_size=%u, pcm_first%u_hex=%s%s",
             sequence,
             (unsigned int)flags,
             (unsigned int)payload_bytes,
             (unsigned int)preview_len,
             hex,
             payload_bytes > preview_len ? " ..." : "");
#else
    (void)pcm_payload;
    (void)payload_bytes;
    (void)sequence;
    (void)flags;
#endif
}

/**
 * @brief 把 WebSocket opcode 转成人类可读名称。
 *
 * 调用方法：ASR 协议日志和旧版帧日志都需要把 manual_ws_frame_t opcode 打成可读字符串，
 * 因此函数本身不受调试宏保护，避免某个日志宏开启时出现未声明问题。
 */
#if MIC_ASR_DOUBAO_ENABLE_PROTOCOL_DEBUG_LOG || MIC_ASR_DOUBAO_ENABLE_FRAME_DEBUG_LOG
static const char *mic_asr_doubao_ws_opcode_name(uint8_t opcode)
{
    switch (opcode) {
    case MANUAL_WS_OPCODE_CONTINUATION:
        return "continuation";
    case MANUAL_WS_OPCODE_TEXT:
        return "text";
    case MANUAL_WS_OPCODE_BINARY:
        return "binary";
    case MANUAL_WS_OPCODE_CLOSE:
        return "close";
    case MANUAL_WS_OPCODE_PING:
        return "ping";
    case MANUAL_WS_OPCODE_PONG:
        return "pong";
    default:
        return "unknown";
    }
}
#endif

#if MIC_ASR_DOUBAO_ENABLE_FRAME_DEBUG_LOG
/**
 * @brief 打印 payload 前 32 字节十六进制预览。
 *
 * 调用方法：发送和接收 WebSocket payload 时调用，只打印前 32 字节，避免音频包刷屏。
 */
static void mic_asr_doubao_log_payload_hex_preview(const char *prefix,
                                                   const uint8_t *payload,
                                                   size_t payload_len)
{
    if (prefix == NULL) {
        return;
    }

    if (payload == NULL || payload_len == 0) {
        ESP_LOGI(TAG, "%s payload_first32_hex=<empty>", prefix);
        return;
    }

    size_t preview_len = payload_len;
    if (preview_len > MIC_ASR_DOUBAO_WS_PAYLOAD_HEX_PREVIEW_BYTES) {
        preview_len = MIC_ASR_DOUBAO_WS_PAYLOAD_HEX_PREVIEW_BYTES;
    }

    char hex[MIC_ASR_DOUBAO_WS_PAYLOAD_HEX_PREVIEW_BYTES * 3 + 1] = {0};
    size_t used = 0;
    for (size_t i = 0; i < preview_len && used + 3 < sizeof(hex); i++) {
        int written = snprintf(&hex[used],
                               sizeof(hex) - used,
                               "%02X%s",
                               (unsigned int)payload[i],
                               (i + 1 < preview_len) ? " " : "");
        if (written <= 0) {
            break;
        }
        used += (size_t)written;
    }

    ESP_LOGI(TAG,
             "%s payload_first32_hex=%s%s",
             prefix,
             hex,
             payload_len > preview_len ? " ..." : "");
}
#endif

/**
 * @brief 生成 full client request 的 JSON payload。
 *
 * 调用方法：WebSocket 连接成功后调用一次，再用 0x11 0x10 0x10 0x00
 * 豆包协议头作为二进制 WebSocket payload 发送。
 */
static void mic_asr_doubao_make_full_client_request_json(char *body, size_t body_len)
{
    snprintf(body,
             body_len,
             "{\"audio\":{\"format\":\"pcm\",\"codec\":\"raw\",\"rate\":%d,\"bits\":%d,\"channel\":%d},"
             "\"request\":{\"language\":\"%s\",\"model_name\":\"%s\"}}",
             MIC_ASR_DOUBAO_SAMPLE_RATE,
             MIC_ASR_DOUBAO_BITS,
             MIC_ASR_DOUBAO_CHANNELS,
             MIC_ASR_DOUBAO_LANGUAGE,
             MIC_ASR_DOUBAO_MODEL_NAME);
}

/**
 * @brief 发送已经写入 frame_buffer 的豆包 WebSocket binary message。
 *
 * 调用方法：full client request 和 audio only request 完成协议前缀组装后调用。
 * WebSocket opcode/mask/长度字段由 manual_ws_send_binary() 处理。
 */
static esp_err_t mic_asr_doubao_send_current_frame(const char *send_label, size_t frame_len)
{
    if (s_asr.connection_broken) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_asr.ws.tls == NULL || frame_len == 0 || frame_len > sizeof(s_asr.frame_buffer)) {
        return ESP_ERR_INVALID_STATE;
    }

#if MIC_ASR_DOUBAO_ENABLE_PROTOCOL_DEBUG_LOG
    mic_asr_doubao_log_protocol_header(send_label, s_asr.frame_buffer, frame_len);
    mic_asr_doubao_log_hex_preview("ASR SEND protocol_prefix",
                                   s_asr.frame_buffer,
                                   frame_len < MIC_ASR_DOUBAO_AUDIO_REQUEST_PREFIX_BYTES ?
                                   frame_len : MIC_ASR_DOUBAO_AUDIO_REQUEST_PREFIX_BYTES,
                                   MIC_ASR_DOUBAO_AUDIO_REQUEST_PREFIX_BYTES);
#else
    (void)send_label;
#endif
#if MIC_ASR_DOUBAO_ENABLE_FRAME_DEBUG_LOG
    ESP_LOGI(TAG,
             "ASR WS SEND opcode=0x%02X(%s) payload_len=%u transport=manual_ws",
             MANUAL_WS_OPCODE_BINARY,
             mic_asr_doubao_ws_opcode_name(MANUAL_WS_OPCODE_BINARY),
             (unsigned int)frame_len);
    mic_asr_doubao_log_payload_hex_preview("ASR WS SEND", s_asr.frame_buffer, frame_len);
#endif

    esp_err_t ret = manual_ws_send_binary(&s_asr.ws, s_asr.frame_buffer, frame_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ASR WS send failed: %s", esp_err_to_name(ret));
        mic_asr_doubao_mark_connection_broken("manual_ws_send_binary failed");
    }
    return ret;
}

/**
 * @brief 按豆包二进制协议封装并发送 FULL_CLIENT_REQUEST。
 *
 * 调用方法：manual_ws_connect() 成功后调用一次。FULL_CLIENT_REQUEST 保持 JSON payload，
 * flags=0，不携带 sequence，帧结构固定为 4 字节协议头 + 4 字节 payload_size + JSON。
 */
static esp_err_t mic_asr_doubao_send_full_client_frame(const uint8_t *payload, size_t payload_len)
{
    if (s_asr.ws.tls == NULL || payload == NULL || payload_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (payload_len > sizeof(s_asr.frame_buffer) - MIC_ASR_DOUBAO_FULL_REQUEST_PREFIX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(s_asr.frame_buffer,
           MIC_ASR_DOUBAO_HEADER_FULL_CLIENT_REQUEST,
           MIC_ASR_DOUBAO_PROTOCOL_HEADER_BYTES);
    mic_asr_doubao_u32_to_be_bytes((uint32_t)payload_len,
                                   &s_asr.frame_buffer[MIC_ASR_DOUBAO_PROTOCOL_HEADER_BYTES]);
    memcpy(&s_asr.frame_buffer[MIC_ASR_DOUBAO_FULL_REQUEST_PREFIX_BYTES], payload, payload_len);

    return mic_asr_doubao_send_current_frame("ASR SEND FULL_CLIENT_REQUEST",
                                             MIC_ASR_DOUBAO_FULL_REQUEST_PREFIX_BYTES + payload_len);
}

/**
 * @brief 按豆包二进制协议封装并发送 AUDIO_ONLY_REQUEST。
 *
 * 调用方法：普通音频包传 is_last=false，sequence 从
 * MIC_ASR_DOUBAO_FIRST_AUDIO_SEQUENCE 开始递增；最后一块真实 PCM 传 is_last=true，
 * sequence 写成负数，flags=0x3。帧结构固定为
 * 4 字节协议头 + 4 字节 sequence + 4 字节 payload_size + PCM payload。
 */
static esp_err_t mic_asr_doubao_send_audio_frame(const int16_t *pcm_payload,
                                                 size_t payload_bytes,
                                                 int32_t sequence,
                                                 bool is_last)
{
    if (!s_asr.session_ready || s_asr.ws.tls == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pcm_payload == NULL || payload_bytes == 0 || (payload_bytes % sizeof(int16_t)) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (payload_bytes > MIC_ASR_DOUBAO_PACKET_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!is_last && sequence <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (is_last && sequence >= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t send_flags = is_last ?
        MIC_ASR_DOUBAO_PARSER_FLAG_NEG_SEQUENCE :
        MIC_ASR_DOUBAO_PARSER_FLAG_POS_SEQUENCE;
    mic_asr_doubao_log_audio_send_debug(pcm_payload, payload_bytes, sequence, send_flags);

    const uint8_t *header = is_last ?
        MIC_ASR_DOUBAO_HEADER_LAST_AUDIO_ONLY_REQUEST :
        MIC_ASR_DOUBAO_HEADER_AUDIO_ONLY_REQUEST;
    memcpy(s_asr.frame_buffer, header, MIC_ASR_DOUBAO_PROTOCOL_HEADER_BYTES);
    mic_asr_doubao_i32_to_be_bytes(sequence,
                                   &s_asr.frame_buffer[MIC_ASR_DOUBAO_PROTOCOL_HEADER_BYTES]);
    mic_asr_doubao_u32_to_be_bytes((uint32_t)payload_bytes,
                                   &s_asr.frame_buffer[MIC_ASR_DOUBAO_PROTOCOL_HEADER_BYTES +
                                                       MIC_ASR_DOUBAO_PROTOCOL_SEQUENCE_BYTES]);
    if ((const uint8_t *)pcm_payload != &s_asr.frame_buffer[MIC_ASR_DOUBAO_AUDIO_REQUEST_PREFIX_BYTES]) {
        memcpy(&s_asr.frame_buffer[MIC_ASR_DOUBAO_AUDIO_REQUEST_PREFIX_BYTES],
               pcm_payload,
               payload_bytes);
    }

    const char *label = is_last ?
        "ASR SEND LAST_AUDIO_ONLY_REQUEST" :
        "ASR SEND AUDIO_ONLY_REQUEST";
    return mic_asr_doubao_send_current_frame(label,
                                             MIC_ASR_DOUBAO_AUDIO_REQUEST_PREFIX_BYTES + payload_bytes);
}

/**
 * @brief 发送 full client request。
 *
 * 调用方法：manual_ws_connect() 成功后调用一次，通知豆包本次音频格式和识别参数。
 */
static esp_err_t mic_asr_doubao_send_full_client_request(void)
{
    char json_body[MIC_ASR_DOUBAO_JSON_MAX_LEN] = {0};
    mic_asr_doubao_make_full_client_request_json(json_body, sizeof(json_body));

    return mic_asr_doubao_send_full_client_frame((const uint8_t *)json_body, strlen(json_body));
}

/**
 * @brief 打印本次 ASR 会话使用的音频参数。
 *
 * 调用方法：mic_asr_doubao_start() 在 full client request 成功后调用一次。日志只描述
 * PCM 格式，不包含任何 PCM 原始样本，方便确认设备端和豆包请求 JSON 中的音频参数一致。
 */
static void mic_asr_doubao_log_audio_format(void)
{
    ESP_LOGI(TAG,
             "ASR audio format: signed int16 little-endian PCM, sample_rate=%d, bits=%d, channels=%d, packet_ms=%d, packet_bytes=%d",
             MIC_ASR_DOUBAO_SAMPLE_RATE,
             MIC_ASR_DOUBAO_BITS,
             MIC_ASR_DOUBAO_CHANNELS,
             MIC_ASR_DOUBAO_PACKET_MS,
             MIC_ASR_DOUBAO_PACKET_BYTES);
}

/**
 * @brief 发送当前累计的音频 payload。
 *
 * 调用方法：send_pcm() 中已有一个完整 100 ms 包且新 PCM 到来时，先把旧包作为普通
 * AUDIO_ONLY_REQUEST 发出；finish() 把当前保留的真实 PCM 作为 LAST_AUDIO_ONLY_REQUEST
 * 发出。函数绝不发送 payload_size=0 的空最后包。
 * 若本地 RMS VAD 判断当前包已经到达端点，即使调用方传入 is_last=false，也会把当前
 * 真实 PCM 包改为 LAST_AUDIO_ONLY_REQUEST 发送，并设置 local_audio_finished，后续
 * mic_asr_doubao_send_pcm() 不再继续发送新音频。
 *
 * sequence 计算方法：packet_id 仍按音频业务包从 1 开始计数，但豆包服务端的
 * autoAssignedSequence 在 FULL_CLIENT_REQUEST/首个 FULL_SERVER_RESPONSE 后已经推进到 2，
 * 所以 packet_id=1 必须发送 sequence=2，packet_id=2 发送 sequence=3。最后一包使用
 * 同一个序列值取负数表示结束。
 */
static esp_err_t mic_asr_doubao_flush_pending_audio(bool is_last)
{
    if (s_asr.local_audio_finished) {
        s_asr.pending_audio_bytes = 0;
        return ESP_OK;
    }

    size_t payload_bytes = s_asr.pending_audio_bytes;
    if (payload_bytes == 0) {
        ESP_LOGW(TAG, "ASR no pending PCM payload, skip empty last audio packet");
        return ESP_OK;
    }

    int16_t *payload = (int16_t *)&s_asr.frame_buffer[MIC_ASR_DOUBAO_AUDIO_REQUEST_PREFIX_BYTES];
    uint32_t packet_index = s_asr.sent_audio_packet_count + 1;
    uint32_t sequence_abs_u32 = packet_index + (uint32_t)MIC_ASR_DOUBAO_FIRST_AUDIO_SEQUENCE - 1U;
    if (sequence_abs_u32 > (uint32_t)INT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    int32_t sequence_abs = (int32_t)sequence_abs_u32;
    uint32_t pcm_rms = mic_asr_doubao_calc_pcm_rms(payload, payload_bytes);
    bool local_vad_last = !is_last && mic_asr_doubao_update_local_vad(packet_index,
                                                                      sequence_abs,
                                                                      pcm_rms);
    bool send_as_last = is_last || local_vad_last;
    int32_t sequence = send_as_last ? -sequence_abs : sequence_abs;
    uint8_t send_flags = send_as_last ?
        MIC_ASR_DOUBAO_PARSER_FLAG_NEG_SEQUENCE :
        MIC_ASR_DOUBAO_PARSER_FLAG_POS_SEQUENCE;
#if !MIC_ASR_DOUBAO_DEBUG_PCM_SEND_SIZE
    (void)send_flags;
#endif

    mic_asr_doubao_log_pcm_packet_quality(payload, payload_bytes, packet_index);

#if MIC_ASR_DOUBAO_DEBUG_PCM_SEND_SIZE
    ESP_LOGI(TAG,
             "ASR PCM send prepare packet_id=%" PRIu32 ", sequence=%" PRId32 ", flags=0x%X, payload_bytes=%u",
             packet_index,
             sequence,
             (unsigned int)send_flags,
             (unsigned int)payload_bytes);
#endif

    esp_err_t ret = mic_asr_doubao_send_audio_frame(payload, payload_bytes, sequence, send_as_last);
    if (ret == ESP_OK) {
        s_asr.sent_audio_packet_count = packet_index;
        s_asr.pcm_quality_packet_count = packet_index;
        s_asr.sent_audio_bytes += payload_bytes;
        if (send_as_last) {
            s_asr.local_audio_finished = true;
            s_asr.vad_state = MIC_ASR_DOUBAO_VAD_ENDING;
        }
#if MIC_ASR_DOUBAO_DEBUG_PCM_SEND_SIZE
        ESP_LOGI(TAG,
                 "ASR PCM send packet_id=%" PRIu32 ", sequence=%" PRId32 ", flags=0x%X, payload_bytes=%u, total_sent_bytes=%u",
                 packet_index,
                 sequence,
                 (unsigned int)send_flags,
                 (unsigned int)payload_bytes,
                 (unsigned int)s_asr.sent_audio_bytes);
#endif
        if (MIC_ASR_DOUBAO_PACKET_SEND_DELAY_MS > 0) {
            vTaskDelay(pdMS_TO_TICKS(MIC_ASR_DOUBAO_PACKET_SEND_DELAY_MS));
        }
        ret = mic_asr_doubao_poll_server_once(MIC_ASR_DOUBAO_SEND_POLL_TIMEOUT_MS);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "ASR poll server after packet failed: %s", esp_err_to_name(ret));
        }
        s_asr.pending_audio_bytes = 0;
    }
    return ret;
}

/**
 * @brief 安全读取 JSON int32 字段。
 *
 * 调用方法：解析 utterance.start_time/end_time 时调用。字段缺失、非数字或越界都返回
 * false，让调用方按无效 utterance 处理。
 */
static bool mic_asr_doubao_get_json_i32(const cJSON *object, const char *name, int32_t *out_value)
{
    if (object == NULL || name == NULL || out_value == NULL) {
        return false;
    }

    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item) || item->valuedouble < (double)INT32_MIN ||
        item->valuedouble > (double)INT32_MAX) {
        return false;
    }

    *out_value = (int32_t)item->valuedouble;
    return true;
}

/**
 * @brief 从 utterance/result/root 三层中选取 log_id。
 *
 * 调用方法：输出 ASR FINAL 和做 final 去重前调用。豆包不同响应里 log_id 可能位于
 * utterance、result 或顶层；优先使用更靠近 utterance 的字段，缺失时使用空字符串。
 */
static const char *mic_asr_doubao_pick_log_id(const cJSON *root,
                                              const cJSON *result,
                                              const cJSON *utterance)
{
    const cJSON *log_id = cJSON_GetObjectItemCaseSensitive(utterance, "log_id");
    if (!cJSON_IsString(log_id) || log_id->valuestring == NULL) {
        log_id = cJSON_GetObjectItemCaseSensitive(result, "log_id");
    }
    if (!cJSON_IsString(log_id) || log_id->valuestring == NULL) {
        log_id = cJSON_GetObjectItemCaseSensitive(root, "log_id");
    }
    return (cJSON_IsString(log_id) && log_id->valuestring != NULL) ? log_id->valuestring : "";
}

/**
 * @brief 判断 final utterance 是否已经输出过。
 *
 * 调用方法：收到 definite=true 且字段有效的 utterance 后调用。去重 key 固定为
 * log_id + start_time + end_time + text，避免 streaming 响应重复触发下游逻辑。
 */
static bool mic_asr_doubao_final_seen(const char *log_id,
                                      int32_t start_time,
                                      int32_t end_time,
                                      const char *text)
{
    if (log_id == NULL || text == NULL) {
        return false;
    }

    for (size_t i = 0; i < MIC_ASR_DOUBAO_FINAL_DEDUP_HISTORY; i++) {
        const mic_asr_doubao_final_key_t *entry = &s_asr.final_history[i];
        if (entry->used &&
            entry->start_time == start_time &&
            entry->end_time == end_time &&
            strcmp(entry->log_id, log_id) == 0 &&
            strcmp(entry->text, text) == 0) {
            return true;
        }
    }
    return false;
}

/**
 * @brief 记录已经输出过的 final utterance。
 *
 * 调用方法：ASR FINAL 日志和 final 回调触发后调用。使用环形历史保存最近 N 条，
 * N 由 MIC_ASR_DOUBAO_FINAL_DEDUP_HISTORY 配置。
 */
static void mic_asr_doubao_remember_final(const char *log_id,
                                          int32_t start_time,
                                          int32_t end_time,
                                          const char *text)
{
    mic_asr_doubao_final_key_t *entry = &s_asr.final_history[s_asr.final_history_next];
    entry->used = true;
    entry->start_time = start_time;
    entry->end_time = end_time;
    strlcpy(entry->log_id, log_id != NULL ? log_id : "", sizeof(entry->log_id));
    strlcpy(entry->text, text != NULL ? text : "", sizeof(entry->text));
    s_asr.final_history_next =
        (s_asr.final_history_next + 1U) % MIC_ASR_DOUBAO_FINAL_DEDUP_HISTORY;
}

/**
 * @brief 处理 interim ASR 文本。
 *
 * 调用方法：utterance.definite=false 且文本/时间有效时调用。interim 只用于提示，
 * 仅在文本变化时打印 ASR INTERIM，绝不触发 mic_asr_on_final_text() 或下游 LLM。
 */
static void mic_asr_doubao_handle_interim_text(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }
    if (strcmp(s_asr.interim_text, text) == 0) {
        return;
    }

    strlcpy(s_asr.interim_text, text, sizeof(s_asr.interim_text));
#if MIC_ASR_DOUBAO_ENABLE_INTERIM_LOG
    ESP_LOGI(TAG, "ASR INTERIM: text=\"%s\"", text);
#endif
}

/**
 * @brief 处理 final ASR 文本。
 *
 * 调用方法：utterance.definite=true 且文本/时间有效时调用。函数会先按
 * log_id/start/end/text 去重；首次出现时打印干净的 ASR FINAL，保存 recognized_text，
 * 设置 response_done，并调用 mic_asr_on_final_text() 占位回调。
 */
static void mic_asr_doubao_handle_final_text(const char *log_id,
                                             int32_t start_time,
                                             int32_t end_time,
                                             const char *text)
{
    if (text == NULL || text[0] == '\0' || start_time < 0 || end_time < 0) {
        return;
    }

    const char *safe_log_id = log_id != NULL ? log_id : "";
    if (mic_asr_doubao_final_seen(safe_log_id, start_time, end_time, text)) {
        return;
    }

    mic_asr_doubao_remember_final(safe_log_id, start_time, end_time, text);
    strlcpy(s_asr.recognized_text, text, sizeof(s_asr.recognized_text));
    s_asr.response_done = true;
    ESP_LOGI(TAG,
             "ASR FINAL: text=\"%s\", start=%" PRId32 ", end=%" PRId32 ", log_id=%s",
             text,
             start_time,
             end_time,
             safe_log_id[0] != '\0' ? safe_log_id : "<empty>");
    mic_asr_on_final_text(s_asr.recognized_text);
}

/**
 * @brief 解析 result.utterances[] 并区分 interim/final。
 *
 * 调用方法：收到 FULL_SERVER_RESPONSE 的 JSON payload 后调用。返回 true 表示当前响应
 * 包含 utterances 数组，调用方无需再走旧版 result.text 兜底。
 */
static bool mic_asr_doubao_handle_utterances(const cJSON *root, const cJSON *result)
{
    if (!cJSON_IsObject(result)) {
        return false;
    }

    const cJSON *utterances = cJSON_GetObjectItemCaseSensitive(result, "utterances");
    if (!cJSON_IsArray(utterances)) {
        return false;
    }

    const cJSON *utterance = NULL;
    cJSON_ArrayForEach(utterance, utterances) {
        if (!cJSON_IsObject(utterance)) {
            continue;
        }

        const cJSON *text_item = cJSON_GetObjectItemCaseSensitive(utterance, "text");
        if (!cJSON_IsString(text_item) ||
            text_item->valuestring == NULL ||
            text_item->valuestring[0] == '\0') {
            continue;
        }

        int32_t start_time = -1;
        int32_t end_time = -1;
        if (!mic_asr_doubao_get_json_i32(utterance, "start_time", &start_time) ||
            !mic_asr_doubao_get_json_i32(utterance, "end_time", &end_time) ||
            start_time < 0 ||
            end_time < 0) {
            continue;
        }

        const cJSON *definite_item = cJSON_GetObjectItemCaseSensitive(utterance, "definite");
        bool definite = cJSON_IsBool(definite_item) && cJSON_IsTrue(definite_item);
        if (definite) {
            const char *log_id = mic_asr_doubao_pick_log_id(root, result, utterance);
            mic_asr_doubao_handle_final_text(log_id,
                                             start_time,
                                             end_time,
                                             text_item->valuestring);
        } else {
            mic_asr_doubao_handle_interim_text(text_item->valuestring);
        }
    }

    return true;
}

/**
 * @brief 从旧版 result.text/top-level text 读取兼容文本。
 *
 * 调用方法：服务端 JSON 没有 result.utterances[] 时调用。该兜底只更新输出缓冲和
 * ASR INTERIM 变化日志，不触发 final 回调，避免把 streaming interim 当最终结果。
 */
static void mic_asr_doubao_handle_legacy_text_fallback(const cJSON *root, const cJSON *result)
{
    const cJSON *text_item = NULL;
    if (cJSON_IsObject(result)) {
        text_item = cJSON_GetObjectItemCaseSensitive(result, "text");
    }
    if (!cJSON_IsString(text_item) || text_item->valuestring == NULL) {
        text_item = cJSON_GetObjectItemCaseSensitive(root, "text");
    }
    if (cJSON_IsString(text_item) &&
        text_item->valuestring != NULL &&
        text_item->valuestring[0] != '\0') {
        strlcpy(s_asr.recognized_text,
                text_item->valuestring,
                sizeof(s_asr.recognized_text));
        mic_asr_doubao_handle_interim_text(text_item->valuestring);
    }
}

/**
 * @brief 从服务端 JSON payload 中提取 ASR 结果和错误字段。
 *
 * 调用方法：解析到一个完整的豆包二进制协议帧后调用。优先解析
 * result.utterances[]：definite=false 只作为 interim，definite=true 才作为 final；
 * 空文本或 start_time/end_time 为负数的尾部占位 utterance 会被忽略。
 */
static void mic_asr_doubao_extract_result_text(const char *json_payload, size_t json_len)
{
    if (json_payload == NULL || json_len == 0) {
        return;
    }

    cJSON *root = cJSON_ParseWithLength(json_payload, json_len);
    if (root == NULL) {
        ESP_LOGW(TAG, "ASR server payload is not valid JSON");
        return;
    }

    const cJSON *duration = cJSON_GetObjectItemCaseSensitive(root, "duration");
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(root, "message");
    bool has_error_text = (cJSON_IsString(error) &&
                           error->valuestring != NULL &&
                           error->valuestring[0] != '\0') ||
                          (cJSON_IsString(message) &&
                           message->valuestring != NULL &&
                           message->valuestring[0] != '\0');
    if (has_error_text || (cJSON_IsNumber(code) && code->valueint != 0)) {
        ESP_LOGW(TAG, "ASR server returned error JSON");
        s_asr.response_error = true;
        s_asr.response_done = true;
        cJSON_Delete(root);
        return;
    }

    const cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (cJSON_IsObject(result)) {
        const cJSON *result_duration = cJSON_GetObjectItemCaseSensitive(result, "duration");
        if (duration == NULL) {
            duration = result_duration;
        }
    }

    if (!mic_asr_doubao_handle_utterances(root, result)) {
        mic_asr_doubao_handle_legacy_text_fallback(root, result);
    }

#if MIC_ASR_DOUBAO_ENABLE_RESULT_JSON_DEBUG_LOG
    if (cJSON_IsNumber(duration)) {
        ESP_LOGI(TAG,
                 "ASR server result: duration=%d, result.text=%s",
                 duration->valueint,
                 s_asr.recognized_text[0] != '\0' ? s_asr.recognized_text : "<empty>");
    } else if (cJSON_IsString(duration) && duration->valuestring != NULL) {
        ESP_LOGI(TAG,
                 "ASR server result: duration=%s, result.text=%s",
                 duration->valuestring,
                 s_asr.recognized_text[0] != '\0' ? s_asr.recognized_text : "<empty>");
    } else {
        ESP_LOGI(TAG,
                 "ASR server result: duration=<missing>, result.text=%s",
                 s_asr.recognized_text[0] != '\0' ? s_asr.recognized_text : "<empty>");
    }
#else
    (void)duration;
#endif

    cJSON_Delete(root);
}

/**
 * @brief 解析一个完整的服务端 WebSocket binary payload，并提取 result.text。
 *
 * 调用方法：manual_ws_recv_frame() 收到 binary message 后调用。完整的豆包
 * 二进制协议结构由 mic_asr_doubao_parser_parse() 独立解析并打印；本函数只在
 * parser 解析通过后，针对未压缩 FULL_SERVER_RESPONSE 提取 result.text，
 * 不再把 SERVER_ACK 的 sequence 误当作 payload size。
 */
static void mic_asr_doubao_parse_server_payload(const uint8_t *data, size_t len)
{
    if (data == NULL || len < MIC_ASR_DOUBAO_FULL_REQUEST_PREFIX_BYTES) {
        return;
    }

    esp_err_t parser_ret = mic_asr_doubao_parser_parse(data, len);
    if (parser_ret != ESP_OK) {
        mic_asr_doubao_mark_connection_broken("server protocol parser returned error");
        return;
    }

    size_t offset = 0;
    while (offset < len) {
        if (len - offset < MIC_ASR_DOUBAO_PARSER_MIN_FRAME_BYTES) {
            ESP_LOGW(TAG, "ASR server payload trailing bytes: %u", (unsigned int)(len - offset));
            mic_asr_doubao_mark_connection_broken("server payload trailing bytes");
            return;
        }

        uint8_t header_words = data[offset] & MIC_ASR_DOUBAO_PARSER_HEADER_SIZE_MASK;
        size_t header_bytes = (size_t)header_words * MIC_ASR_DOUBAO_PARSER_HEADER_WORD_BYTES;
        if (header_bytes < MIC_ASR_DOUBAO_PARSER_HEADER_BYTES ||
            len - offset < header_bytes + MIC_ASR_DOUBAO_PARSER_PAYLOAD_SIZE_BYTES) {
            ESP_LOGW(TAG, "ASR server frame header invalid while extracting result");
            mic_asr_doubao_mark_connection_broken("server frame header invalid");
            return;
        }

        uint8_t message_type =
            (data[offset + 1] & MIC_ASR_DOUBAO_PARSER_MSG_TYPE_MASK) >>
            MIC_ASR_DOUBAO_PARSER_MSG_TYPE_SHIFT;
        uint8_t flags = data[offset + 1] & MIC_ASR_DOUBAO_PARSER_FLAGS_MASK;
        uint8_t serialization =
            (data[offset + 2] & MIC_ASR_DOUBAO_PARSER_SERIAL_MASK) >>
            MIC_ASR_DOUBAO_PARSER_SERIAL_SHIFT;
        uint8_t compression = data[offset + 2] & MIC_ASR_DOUBAO_PARSER_COMPRESS_MASK;

        size_t cursor = offset + header_bytes;
        bool has_sequence = flags == MIC_ASR_DOUBAO_PARSER_FLAG_POS_SEQUENCE ||
                            flags == MIC_ASR_DOUBAO_PARSER_FLAG_NEG_SEQUENCE;
        int32_t sequence = 0;
        if (has_sequence) {
            if (len - cursor < MIC_ASR_DOUBAO_PARSER_SEQUENCE_BYTES +
                               MIC_ASR_DOUBAO_PARSER_PAYLOAD_SIZE_BYTES) {
                ESP_LOGW(TAG, "ASR server sequence field invalid while extracting result");
                mic_asr_doubao_mark_connection_broken("server sequence field invalid");
                return;
            }
            sequence = mic_asr_doubao_i32_from_be_bytes(&data[cursor]);
            cursor += MIC_ASR_DOUBAO_PARSER_SEQUENCE_BYTES;
        }

        uint32_t payload_len = mic_asr_doubao_u32_from_be_bytes(&data[cursor]);
        cursor += MIC_ASR_DOUBAO_PARSER_PAYLOAD_SIZE_BYTES;
        if ((size_t)payload_len > len - cursor) {
            ESP_LOGW(TAG,
                     "ASR server frame length invalid while extracting result: payload=%" PRIu32 ", available=%u",
                     payload_len,
                     (unsigned int)(len - cursor));
            mic_asr_doubao_mark_connection_broken("server frame payload length invalid");
            return;
        }

        if (message_type == MIC_ASR_DOUBAO_PARSER_MSG_FULL_RESPONSE &&
            serialization == MIC_ASR_DOUBAO_PARSER_SERIAL_JSON &&
            compression == MIC_ASR_DOUBAO_PARSER_COMPRESS_NONE &&
            payload_len > 0) {
            const char *json_payload = (const char *)&data[cursor];
            mic_asr_doubao_extract_result_text(json_payload, payload_len);
        }
        if (message_type == MIC_ASR_DOUBAO_PARSER_MSG_SERVER_ERROR) {
            mic_asr_doubao_mark_connection_broken("server returned SERVER_ERROR");
            return;
        }
        if (has_sequence && sequence < 0 && !s_asr.response_done) {
            ESP_LOGI(TAG,
                     "ASR server negative sequence received without text yet: sequence=%d",
                     (int)sequence);
        }

        offset = cursor + (size_t)payload_len;
    }
}

/**
 * @brief 追加 WebSocket continuation 分片并在 FIN 时解析完整业务消息。
 *
 * 调用方法：manual_ws_recv_frame() 返回 binary/continuation 且 FIN=0 或 continuation 时调用。
 */
static esp_err_t mic_asr_doubao_append_fragment(const manual_ws_frame_t *frame)
{
    if (frame == NULL || frame->payload_len == 0) {
        return ESP_OK;
    }
    if (s_asr.rx_message_len + frame->payload_len > MIC_ASR_DOUBAO_RX_MESSAGE_MAX_BYTES) {
        ESP_LOGE(TAG,
                 "ASR rx fragmented message too large: %u + %u",
                 (unsigned int)s_asr.rx_message_len,
                 (unsigned int)frame->payload_len);
        mic_asr_doubao_free_rx_message_buffer();
        mic_asr_doubao_mark_connection_broken("fragmented server message too large");
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *new_buffer = (uint8_t *)realloc(s_asr.rx_message_buffer,
                                             s_asr.rx_message_len + frame->payload_len);
    if (new_buffer == NULL) {
        ESP_LOGE(TAG, "alloc ASR fragmented message buffer failed");
        mic_asr_doubao_free_rx_message_buffer();
        mic_asr_doubao_mark_connection_broken("alloc fragmented message buffer failed");
        return ESP_ERR_NO_MEM;
    }

    s_asr.rx_message_buffer = new_buffer;
    memcpy(&s_asr.rx_message_buffer[s_asr.rx_message_len], frame->payload, frame->payload_len);
    s_asr.rx_message_len += frame->payload_len;
    s_asr.rx_message_active = true;

    if (frame->fin) {
        mic_asr_doubao_parse_server_payload(s_asr.rx_message_buffer, s_asr.rx_message_len);
        mic_asr_doubao_free_rx_message_buffer();
    }
    return ESP_OK;
}

/**
 * @brief 处理 manual_ws 返回的一帧 WebSocket 数据。
 *
 * 调用方法：finish() 等待结果时循环调用。manual_ws 已经处理 mask、ping 自动 pong 和
 * payload 长度解析；这里仅根据 opcode 决定是否进入豆包 ASR 二进制协议解析。
 */
static esp_err_t mic_asr_doubao_handle_ws_frame(const manual_ws_frame_t *frame)
{
    if (frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_asr.rx_ws_frame_count++;
#if MIC_ASR_DOUBAO_ENABLE_PROTOCOL_DEBUG_LOG
    ESP_LOGI(TAG,
             "ASR WS RECV frame=%" PRIu32 " fin=%d opcode=0x%02X(%s) payload_len=%u masked=%d",
             s_asr.rx_ws_frame_count,
             frame->fin,
             (unsigned int)frame->opcode,
             mic_asr_doubao_ws_opcode_name(frame->opcode),
             (unsigned int)frame->payload_len,
             frame->masked);
    if (frame->opcode == MANUAL_WS_OPCODE_BINARY && frame->payload_len > 0) {
        mic_asr_doubao_log_protocol_header("ASR RECV server frame", frame->payload, frame->payload_len);
    }
#endif
#if MIC_ASR_DOUBAO_ENABLE_FRAME_DEBUG_LOG
    ESP_LOGI(TAG,
             "ASR WS RECV fin=%d opcode=0x%02X(%s) payload_len=%u masked=%d transport=manual_ws",
             frame->fin,
             (unsigned int)frame->opcode,
             mic_asr_doubao_ws_opcode_name(frame->opcode),
             (unsigned int)frame->payload_len,
             frame->masked);
    mic_asr_doubao_log_payload_hex_preview("ASR WS RECV", frame->payload, frame->payload_len);
#endif

    if (frame->opcode == MANUAL_WS_OPCODE_CLOSE) {
        ESP_LOGE(TAG, "ASR WS server close frame");
        mic_asr_doubao_mark_connection_broken("server sent close frame");
        return ESP_OK;
    }

    if (frame->opcode == MANUAL_WS_OPCODE_PING ||
        frame->opcode == MANUAL_WS_OPCODE_PONG) {
        return ESP_OK;
    }

    if (frame->opcode == MANUAL_WS_OPCODE_TEXT) {
#if MIC_ASR_DOUBAO_ENABLE_FRAME_DEBUG_LOG
        ESP_LOGW(TAG, "ASR WS received text frame, skip Doubao binary parser");
#endif
        return ESP_OK;
    }

    if (frame->opcode == MANUAL_WS_OPCODE_BINARY) {
        if (frame->fin && !s_asr.rx_message_active) {
            mic_asr_doubao_parse_server_payload(frame->payload, frame->payload_len);
            return ESP_OK;
        }
        if (s_asr.rx_message_active) {
            ESP_LOGE(TAG, "ASR rx got new binary frame before previous fragmented message ended");
            mic_asr_doubao_free_rx_message_buffer();
            mic_asr_doubao_mark_connection_broken("new binary frame before fragmented message ended");
            return ESP_FAIL;
        }
        return mic_asr_doubao_append_fragment(frame);
    }

    if (frame->opcode == MANUAL_WS_OPCODE_CONTINUATION) {
        if (!s_asr.rx_message_active) {
            ESP_LOGE(TAG, "ASR rx continuation without active fragmented message");
            mic_asr_doubao_mark_connection_broken("continuation without active fragmented message");
            return ESP_FAIL;
        }
        return mic_asr_doubao_append_fragment(frame);
    }

#if MIC_ASR_DOUBAO_ENABLE_FRAME_DEBUG_LOG
    ESP_LOGW(TAG, "ASR WS unknown opcode=0x%02X, ignored", (unsigned int)frame->opcode);
#endif
    return ESP_OK;
}

/**
 * @brief 在发送流程中短时间轮询服务端响应。
 *
 * 调用方法：
 * - FULL_CLIENT_REQUEST 发送后调用一次，给服务端 ACK/错误一个短时间窗口；
 * - 每个 100 ms 音频包发送并节流后调用一次，实现边发边收；
 * - finish() 等待最终结果时循环调用。
 *
 * timeout_ms 为 0 时只做一次极短超时设置；ESP_ERR_TIMEOUT 表示当前没有服务端帧，
 * 不是错误。其它 recv/解析错误会把连接标记为 broken，让上层立即停止发送。
 */
static esp_err_t mic_asr_doubao_poll_server_once(int timeout_ms)
{
    if (s_asr.connection_broken) {
        return ESP_FAIL;
    }
    if (s_asr.ws.tls == NULL || !s_asr.ws.connected) {
        mic_asr_doubao_mark_connection_broken("poll server on disconnected websocket");
        return ESP_ERR_INVALID_STATE;
    }

    manual_ws_frame_t frame = {0};
    esp_err_t ret = manual_ws_recv_frame(&s_asr.ws,
                                         &frame,
                                         s_asr.rx_frame_buffer,
                                         sizeof(s_asr.rx_frame_buffer),
                                         timeout_ms);
    if (ret == ESP_ERR_TIMEOUT) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "manual_ws recv frame failed while polling: %s", esp_err_to_name(ret));
        mic_asr_doubao_mark_connection_broken("recv frame failed while polling");
        return ret;
    }

    ret = mic_asr_doubao_handle_ws_frame(&frame);
    if (ret != ESP_OK) {
        mic_asr_doubao_mark_connection_broken("handle server frame failed while polling");
        return ret;
    }
    if (s_asr.connection_broken || s_asr.response_error) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t mic_asr_doubao_start(void)
{
    if (s_asr.session_ready || s_asr.ws.tls != NULL) {
        ESP_LOGW(TAG, "previous ASR session is still active, stop it first");
        mic_asr_doubao_stop();
    }

    mic_asr_doubao_reset_session_state();
    mic_asr_doubao_log_heap("ASR start before");

    mic_asr_doubao_prepare_connect_id();
    mic_asr_doubao_log_config();

    esp_err_t ret = mic_asr_doubao_build_headers();
    if (ret != ESP_OK) {
        return ret;
    }

    manual_ws_config_t ws_config = {
        .host = MIC_ASR_DOUBAO_WS_HOST,
        .path = MIC_ASR_DOUBAO_WS_PATH,
        .port = MIC_ASR_DOUBAO_WS_PORT,
        .timeout_ms = MANUAL_WS_DEFAULT_TIMEOUT_MS,
        .extra_headers = s_asr.headers,
    };

    mic_asr_doubao_log_heap("manual_ws connect before");
    ret = manual_ws_connect(&s_asr.ws, &ws_config);
    mic_asr_doubao_log_heap("manual_ws connect after");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "manual_ws connect failed: %s", esp_err_to_name(ret));
        mic_asr_doubao_stop();
        return ret;
    }

    mic_asr_doubao_log_audio_format();
    ret = mic_asr_doubao_send_full_client_request();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "send full client request failed: %s", esp_err_to_name(ret));
        mic_asr_doubao_stop();
        return ret;
    }

    s_asr.session_ready = true;
    ret = mic_asr_doubao_poll_server_once(MIC_ASR_DOUBAO_POST_FULL_RECV_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ASR server returned error after full client request: %s", esp_err_to_name(ret));
        mic_asr_doubao_stop();
        return ret;
    }
    return ESP_OK;
}

esp_err_t mic_asr_doubao_send_pcm(const int16_t *pcm, size_t bytes)
{
    if (bytes == 0) {
        return ESP_OK;
    }
    if (pcm == NULL || (bytes % sizeof(int16_t)) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_asr.connection_broken || s_asr.response_error) {
        ESP_LOGW(TAG, "ASR connection is broken or has server error, stop sending PCM");
        return ESP_FAIL;
    }
    if (!s_asr.session_ready || s_asr.ws.tls == NULL || !s_asr.ws.connected) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_asr.local_audio_finished) {
        esp_err_t ret = mic_asr_doubao_poll_server_once(MIC_ASR_DOUBAO_SEND_POLL_TIMEOUT_MS);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "ASR poll server after local VAD end failed: %s", esp_err_to_name(ret));
            return ret;
        }
        return ESP_OK;
    }

    const uint8_t *pcm_bytes = (const uint8_t *)pcm;
    size_t offset = 0;
    while (offset < bytes) {
        if (s_asr.pending_audio_bytes == MIC_ASR_DOUBAO_PACKET_BYTES) {
            esp_err_t ret = mic_asr_doubao_flush_pending_audio(false);
            if (ret != ESP_OK) {
                return ret;
            }
            if (s_asr.connection_broken || s_asr.response_error) {
                return ESP_FAIL;
            }
            if (s_asr.local_audio_finished) {
                return ESP_OK;
            }
        }

        size_t free_bytes = MIC_ASR_DOUBAO_PACKET_BYTES - s_asr.pending_audio_bytes;
        size_t copy_bytes = bytes - offset;
        if (copy_bytes > free_bytes) {
            copy_bytes = free_bytes;
        }

        memcpy(&s_asr.frame_buffer[MIC_ASR_DOUBAO_AUDIO_REQUEST_PREFIX_BYTES + s_asr.pending_audio_bytes],
               &pcm_bytes[offset],
               copy_bytes);
        s_asr.pending_audio_bytes += copy_bytes;
        offset += copy_bytes;
    }

    return ESP_OK;
}

esp_err_t mic_asr_doubao_finish(char *text_buf, size_t text_buf_size)
{
    if (text_buf != NULL && text_buf_size > 0) {
        text_buf[0] = '\0';
    }
    if (s_asr.connection_broken) {
        ESP_LOGW(TAG, "ASR connection already broken, skip last audio packet");
        return ESP_FAIL;
    }
    if (!s_asr.session_ready || s_asr.ws.tls == NULL || !s_asr.ws.connected) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;
    if (!s_asr.local_audio_finished) {
        ret = mic_asr_doubao_flush_pending_audio(true);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "send last audio packet failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }
#if MIC_ASR_DOUBAO_ENABLE_FRAME_DEBUG_LOG
    ESP_LOGI(TAG, "ASR audio sent: %u bytes", (unsigned int)s_asr.sent_audio_bytes);
#endif

    TickType_t start_tick = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(MIC_ASR_DOUBAO_RESPONSE_TIMEOUT_MS);
    while (!s_asr.response_done && (xTaskGetTickCount() - start_tick) < timeout_ticks) {
        TickType_t elapsed_ticks = xTaskGetTickCount() - start_tick;
        TickType_t remaining_ticks = elapsed_ticks < timeout_ticks ? timeout_ticks - elapsed_ticks : 0;
        int remaining_ms = (int)((remaining_ticks * 1000U) / configTICK_RATE_HZ);
        if (remaining_ms <= 0) {
            break;
        }

        ret = mic_asr_doubao_poll_server_once(remaining_ms);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "ASR poll server while finishing failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    if (!s_asr.response_done) {
        ESP_LOGW(TAG, "ASR response timeout");
        return ESP_ERR_TIMEOUT;
    }
    if (s_asr.response_error) {
        return ESP_FAIL;
    }
    if (s_asr.recognized_text[0] == '\0') {
        ESP_LOGI(TAG, "本次未识别到文字");
        return ESP_OK;
    }

    if (text_buf != NULL && text_buf_size > 0) {
        strlcpy(text_buf, s_asr.recognized_text, text_buf_size);
    }
#if MIC_ASR_DOUBAO_ENABLE_RESULT_PRINT
    ESP_LOGI(TAG, "ASR result: %s", s_asr.recognized_text);
    printf("ASR result: %s\n", s_asr.recognized_text);
    fflush(stdout);
#endif
    return ESP_OK;
}

void mic_asr_doubao_stop(void)
{
    if (s_asr.connection_broken && s_asr.ws.tls != NULL) {
        s_asr.ws.connected = false;
    }
    manual_ws_close(&s_asr.ws);
    mic_asr_doubao_reset_session_state();
    mic_asr_doubao_log_heap("ASR stop after");
}
