#include "mic_asr_doubao.h"

#include <inttypes.h>
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
    MIC_ASR_DOUBAO_PROTOCOL_LENGTH_BYTES = 4,
    MIC_ASR_DOUBAO_PROTOCOL_PREFIX_BYTES = 8,
    MIC_ASR_DOUBAO_FRAME_BUFFER_BYTES =
        MIC_ASR_DOUBAO_PROTOCOL_PREFIX_BYTES + MIC_ASR_DOUBAO_PACKET_BYTES,
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
    0x11, 0x20, 0x00, 0x00
};
static const uint8_t MIC_ASR_DOUBAO_HEADER_LAST_AUDIO_ONLY_REQUEST[MIC_ASR_DOUBAO_PROTOCOL_HEADER_BYTES] = {
    0x11, 0x22, 0x00, 0x00
};

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

    char connect_id[MIC_ASR_DOUBAO_CONNECT_ID_MAX_LEN];            // 本次握手的 X-Api-Connect-Id。
    char headers[MIC_ASR_DOUBAO_HEADERS_MAX_LEN];                  // 只保存豆包 X-Api-* 业务 Header。
    char recognized_text[MIC_ASR_DOUBAO_RESULT_TEXT_MAX_LEN];      // 最新识别文本。

    uint8_t frame_buffer[MIC_ASR_DOUBAO_FRAME_BUFFER_BYTES];       // 8 字节豆包协议前缀 + PCM/JSON payload。
    size_t pending_audio_bytes;                                    // frame_buffer 中尚未发送的 PCM 字节数。
    size_t sent_audio_bytes;                                       // 已发送给 ASR 的 PCM 字节数，仅用于日志。
    uint32_t pcm_quality_packet_count;                             // 本会话已经统计过的 send_pcm() 调用次数。
    uint32_t pcm_quality_silence_streak;                           // 连续 p2p 很小的 PCM 包数量，用于判断长期接近静音。

    uint8_t rx_frame_buffer[MANUAL_WS_MAX_PAYLOAD_SIZE];           // manual_ws_recv_frame() 的单帧 payload 缓冲。
    uint8_t *rx_message_buffer;                                    // WebSocket continuation 业务消息重组缓冲。
    size_t rx_message_len;                                         // 当前已累计的业务消息长度。
    bool rx_message_active;                                        // true 表示正在重组一个分片 binary message。
} mic_asr_doubao_session_t;

static mic_asr_doubao_session_t s_asr;

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
    s_asr.pending_audio_bytes = 0;
    s_asr.sent_audio_bytes = 0;
    s_asr.pcm_quality_packet_count = 0;
    s_asr.pcm_quality_silence_streak = 0;
    s_asr.connect_id[0] = '\0';
    s_asr.headers[0] = '\0';
    s_asr.recognized_text[0] = '\0';
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
 * @brief 扫描一次 send_pcm() 传入的 PCM 数据并打印质量统计。
 *
 * 调用方法：mic_asr_doubao_send_pcm() 在真正写入 frame_buffer 前调用。本函数只读取
 * 当前调用传进来的 pcm 指针，按 int16_t little-endian PCM_s16le 统计当前包的
 * sample_count、min、max、avg、rms、p2p，不复制、不暂存、不改变后续发送协议。
 *
 * 由于当前 ESP32-C5 平台按 little-endian 访问 int16_t，这里直接读取 int16_t 样本；
 * 如果后续移植到非 little-endian 平台，应改为逐字节拼出 int16_t。
 *
 * @param pcm PCM_s16le 样本指针，不能为空。
 * @param bytes PCM 字节数，必须是 int16_t 的整数倍。
 */
static void mic_asr_doubao_log_pcm_quality(const int16_t *pcm, size_t bytes)
{
#if MIC_ASR_DOUBAO_ENABLE_PCM_QUALITY_LOG
    if (pcm == NULL || bytes == 0 || (bytes % sizeof(int16_t)) != 0) {
        return;
    }

    size_t sample_count = bytes / sizeof(int16_t);
    int16_t min_sample = INT16_MAX;
    int16_t max_sample = INT16_MIN;
    int64_t sum_samples = 0;
    uint64_t sum_square_samples = 0;
    size_t clip_like_count = 0;

    for (size_t i = 0; i < sample_count; i++) {
        int16_t sample = pcm[i];
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

        sum_samples += sample_i32;
        sum_square_samples += (uint64_t)(sample_i32 * sample_i32);
    }

    int32_t avg_sample = (int32_t)(sum_samples / (int64_t)sample_count);
    uint32_t rms_sample = mic_asr_doubao_isqrt_u64(sum_square_samples / sample_count);
    uint32_t p2p_sample = (uint32_t)((int32_t)max_sample - (int32_t)min_sample);
    uint32_t clip_percent = (uint32_t)((clip_like_count * 100U) / sample_count);

    s_asr.pcm_quality_packet_count++;
    if (p2p_sample <= MIC_ASR_DOUBAO_PCM_SILENCE_P2P_THRESHOLD) {
        s_asr.pcm_quality_silence_streak++;
    } else {
        s_asr.pcm_quality_silence_streak = 0;
    }

    bool should_log_this_packet =
        (s_asr.pcm_quality_packet_count % MIC_ASR_DOUBAO_PCM_QUALITY_LOG_EVERY_PACKETS) == 0;

    if (should_log_this_packet) {
        ESP_LOGI(TAG,
                 "ASR PCM quality packet=%" PRIu32 ", sample_count=%u, min=%d, max=%d, avg=%" PRId32 ", rms=%" PRIu32 ", p2p=%" PRIu32 ", clip_like=%u%%",
                 s_asr.pcm_quality_packet_count,
                 (unsigned int)sample_count,
                 (int)min_sample,
                 (int)max_sample,
                 avg_sample,
                 rms_sample,
                 p2p_sample,
                 (unsigned int)clip_percent);
    }

    if (should_log_this_packet &&
        s_asr.pcm_quality_silence_streak >= MIC_ASR_DOUBAO_PCM_SILENCE_WARN_CONSECUTIVE) {
        ESP_LOGW(TAG,
                 "PCM 可能接近静音: consecutive=%" PRIu32 ", p2p=%" PRIu32 ", rms=%" PRIu32,
                 s_asr.pcm_quality_silence_streak,
                 p2p_sample,
                 rms_sample);
    }

    if (should_log_this_packet && clip_percent >= MIC_ASR_DOUBAO_PCM_CLIP_WARN_PERCENT) {
        ESP_LOGW(TAG,
                 "PCM 可能削波: min=%d, max=%d, clip_like=%u%%",
                 (int)min_sample,
                 (int)max_sample,
                 (unsigned int)clip_percent);
    }
#else
    (void)pcm;
    (void)bytes;
#endif
}

#if MIC_ASR_DOUBAO_ENABLE_FRAME_DEBUG_LOG
/**
 * @brief 把 WebSocket opcode 转成人类可读名称。
 *
 * 调用方法：ASR 帧调试日志开启时，把 manual_ws_frame_t opcode 打成可读字符串。
 */
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
 * @brief 按豆包二进制协议封装并发送一帧 WebSocket binary message。
 *
 * 调用方法：full client request、普通 audio only request、last audio packet 都通过
 * 本函数发送。WebSocket opcode/mask/长度字段由 manual_ws_send_binary() 处理。
 */
static esp_err_t mic_asr_doubao_send_binary_frame(const uint8_t protocol_header[MIC_ASR_DOUBAO_PROTOCOL_HEADER_BYTES],
                                                  const uint8_t *payload,
                                                  size_t payload_len)
{
    if (!s_asr.session_ready && protocol_header != MIC_ASR_DOUBAO_HEADER_FULL_CLIENT_REQUEST) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_asr.ws.tls == NULL || protocol_header == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (payload_len > MIC_ASR_DOUBAO_PACKET_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (payload == NULL && payload_len > 0) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(s_asr.frame_buffer, protocol_header, MIC_ASR_DOUBAO_PROTOCOL_HEADER_BYTES);
    mic_asr_doubao_u32_to_be_bytes((uint32_t)payload_len,
                                   &s_asr.frame_buffer[MIC_ASR_DOUBAO_PROTOCOL_HEADER_BYTES]);
    if (payload != NULL &&
        payload_len > 0 &&
        payload != &s_asr.frame_buffer[MIC_ASR_DOUBAO_PROTOCOL_PREFIX_BYTES]) {
        memcpy(&s_asr.frame_buffer[MIC_ASR_DOUBAO_PROTOCOL_PREFIX_BYTES], payload, payload_len);
    }

    size_t frame_len = MIC_ASR_DOUBAO_PROTOCOL_PREFIX_BYTES + payload_len;
#if MIC_ASR_DOUBAO_ENABLE_FRAME_DEBUG_LOG
    ESP_LOGI(TAG,
             "ASR WS SEND opcode=0x%02X(%s) payload_len=%u transport=manual_ws",
             MANUAL_WS_OPCODE_BINARY,
             mic_asr_doubao_ws_opcode_name(MANUAL_WS_OPCODE_BINARY),
             (unsigned int)frame_len);
    mic_asr_doubao_log_payload_hex_preview("ASR WS SEND", s_asr.frame_buffer, frame_len);
#endif

    return manual_ws_send_binary(&s_asr.ws, s_asr.frame_buffer, frame_len);
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

    return mic_asr_doubao_send_binary_frame(MIC_ASR_DOUBAO_HEADER_FULL_CLIENT_REQUEST,
                                            (const uint8_t *)json_body,
                                            strlen(json_body));
}

/**
 * @brief 发送当前累计的音频 payload。
 *
 * 调用方法：send_pcm() 凑满 MIC_ASR_DOUBAO_PACKET_BYTES 字节时发送普通音频包；
 * finish() 发送剩余 PCM 或 0 字节 last audio packet。
 */
static esp_err_t mic_asr_doubao_flush_pending_audio(bool is_last)
{
    const uint8_t *header = is_last ?
        MIC_ASR_DOUBAO_HEADER_LAST_AUDIO_ONLY_REQUEST :
        MIC_ASR_DOUBAO_HEADER_AUDIO_ONLY_REQUEST;
    const uint8_t *payload = NULL;
    if (s_asr.pending_audio_bytes > 0) {
        payload = &s_asr.frame_buffer[MIC_ASR_DOUBAO_PROTOCOL_PREFIX_BYTES];
    }

    esp_err_t ret = mic_asr_doubao_send_binary_frame(header, payload, s_asr.pending_audio_bytes);
    if (ret == ESP_OK) {
        s_asr.sent_audio_bytes += s_asr.pending_audio_bytes;
        s_asr.pending_audio_bytes = 0;
    }
    return ret;
}

/**
 * @brief 从服务端 JSON payload 中提取 result.text。
 *
 * 调用方法：解析到一个完整的豆包二进制协议帧后调用。当前不做 gzip，
 * payload 直接按 JSON 解析。
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

    const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (error != NULL || (cJSON_IsNumber(code) && code->valueint != 0)) {
        ESP_LOGW(TAG, "ASR server returned error JSON");
        s_asr.response_error = true;
        s_asr.response_done = true;
        cJSON_Delete(root);
        return;
    }

    const cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (cJSON_IsObject(result)) {
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(result, "text");
        if (cJSON_IsString(text) && text->valuestring != NULL) {
            strlcpy(s_asr.recognized_text, text->valuestring, sizeof(s_asr.recognized_text));
            s_asr.response_done = true;
        }
    }

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
    if (data == NULL || len < MIC_ASR_DOUBAO_PROTOCOL_PREFIX_BYTES) {
        return;
    }

    esp_err_t parser_ret = mic_asr_doubao_parser_parse(data, len);
    if (parser_ret != ESP_OK && parser_ret != ESP_ERR_NOT_SUPPORTED) {
        s_asr.response_error = true;
        s_asr.response_done = true;
        return;
    }
    if (parser_ret == ESP_ERR_NOT_SUPPORTED) {
        s_asr.response_error = true;
        s_asr.response_done = true;
        return;
    }

    size_t offset = 0;
    while (offset < len) {
        if (len - offset < MIC_ASR_DOUBAO_PARSER_MIN_FRAME_BYTES) {
            ESP_LOGW(TAG, "ASR server payload trailing bytes: %u", (unsigned int)(len - offset));
            s_asr.response_error = true;
            s_asr.response_done = true;
            return;
        }

        uint8_t header_words = data[offset] & MIC_ASR_DOUBAO_PARSER_HEADER_SIZE_MASK;
        size_t header_bytes = (size_t)header_words * MIC_ASR_DOUBAO_PARSER_HEADER_WORD_BYTES;
        if (header_bytes < MIC_ASR_DOUBAO_PARSER_HEADER_BYTES ||
            len - offset < header_bytes + MIC_ASR_DOUBAO_PARSER_PAYLOAD_SIZE_BYTES) {
            ESP_LOGW(TAG, "ASR server frame header invalid while extracting result");
            s_asr.response_error = true;
            s_asr.response_done = true;
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
        if (has_sequence) {
            if (len - cursor < MIC_ASR_DOUBAO_PARSER_SEQUENCE_BYTES +
                               MIC_ASR_DOUBAO_PARSER_PAYLOAD_SIZE_BYTES) {
                ESP_LOGW(TAG, "ASR server sequence field invalid while extracting result");
                s_asr.response_error = true;
                s_asr.response_done = true;
                return;
            }
            cursor += MIC_ASR_DOUBAO_PARSER_SEQUENCE_BYTES;
        }

        uint32_t payload_len = mic_asr_doubao_u32_from_be_bytes(&data[cursor]);
        cursor += MIC_ASR_DOUBAO_PARSER_PAYLOAD_SIZE_BYTES;
        if ((size_t)payload_len > len - cursor) {
            ESP_LOGW(TAG,
                     "ASR server frame length invalid while extracting result: payload=%" PRIu32 ", available=%u",
                     payload_len,
                     (unsigned int)(len - cursor));
            s_asr.response_error = true;
            s_asr.response_done = true;
            return;
        }

        if (message_type == MIC_ASR_DOUBAO_PARSER_MSG_FULL_RESPONSE &&
            serialization == MIC_ASR_DOUBAO_PARSER_SERIAL_JSON &&
            compression == MIC_ASR_DOUBAO_PARSER_COMPRESS_NONE &&
            payload_len > 0) {
            const char *json_payload = (const char *)&data[cursor];
            mic_asr_doubao_extract_result_text(json_payload, payload_len);
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
        s_asr.response_error = true;
        s_asr.response_done = true;
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *new_buffer = (uint8_t *)realloc(s_asr.rx_message_buffer,
                                             s_asr.rx_message_len + frame->payload_len);
    if (new_buffer == NULL) {
        ESP_LOGE(TAG, "alloc ASR fragmented message buffer failed");
        mic_asr_doubao_free_rx_message_buffer();
        s_asr.response_error = true;
        s_asr.response_done = true;
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
        s_asr.response_error = true;
        s_asr.response_done = true;
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
            s_asr.response_error = true;
            s_asr.response_done = true;
            return ESP_FAIL;
        }
        return mic_asr_doubao_append_fragment(frame);
    }

    if (frame->opcode == MANUAL_WS_OPCODE_CONTINUATION) {
        if (!s_asr.rx_message_active) {
            ESP_LOGE(TAG, "ASR rx continuation without active fragmented message");
            s_asr.response_error = true;
            s_asr.response_done = true;
            return ESP_FAIL;
        }
        return mic_asr_doubao_append_fragment(frame);
    }

#if MIC_ASR_DOUBAO_ENABLE_FRAME_DEBUG_LOG
    ESP_LOGW(TAG, "ASR WS unknown opcode=0x%02X, ignored", (unsigned int)frame->opcode);
#endif
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

    ret = mic_asr_doubao_send_full_client_request();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "send full client request failed: %s", esp_err_to_name(ret));
        mic_asr_doubao_stop();
        return ret;
    }

    s_asr.session_ready = true;
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
    if (!s_asr.session_ready || s_asr.ws.tls == NULL || !s_asr.ws.connected) {
        return ESP_ERR_INVALID_STATE;
    }

    mic_asr_doubao_log_pcm_quality(pcm, bytes);

    const uint8_t *pcm_bytes = (const uint8_t *)pcm;
    size_t offset = 0;
    while (offset < bytes) {
        size_t free_bytes = MIC_ASR_DOUBAO_PACKET_BYTES - s_asr.pending_audio_bytes;
        size_t copy_bytes = bytes - offset;
        if (copy_bytes > free_bytes) {
            copy_bytes = free_bytes;
        }

        memcpy(&s_asr.frame_buffer[MIC_ASR_DOUBAO_PROTOCOL_PREFIX_BYTES + s_asr.pending_audio_bytes],
               &pcm_bytes[offset],
               copy_bytes);
        s_asr.pending_audio_bytes += copy_bytes;
        offset += copy_bytes;

        if (s_asr.pending_audio_bytes == MIC_ASR_DOUBAO_PACKET_BYTES) {
            esp_err_t ret = mic_asr_doubao_flush_pending_audio(false);
            if (ret != ESP_OK) {
                return ret;
            }
        }
    }

    return ESP_OK;
}

esp_err_t mic_asr_doubao_finish(char *text_buf, size_t text_buf_size)
{
    if (text_buf != NULL && text_buf_size > 0) {
        text_buf[0] = '\0';
    }
    if (!s_asr.session_ready || s_asr.ws.tls == NULL || !s_asr.ws.connected) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = mic_asr_doubao_flush_pending_audio(true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "send last audio packet failed: %s", esp_err_to_name(ret));
        return ret;
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

        manual_ws_frame_t frame = {0};
        ret = manual_ws_recv_frame(&s_asr.ws,
                                   &frame,
                                   s_asr.rx_frame_buffer,
                                   sizeof(s_asr.rx_frame_buffer),
                                   (int)remaining_ms);
        if (ret == ESP_ERR_TIMEOUT) {
            continue;
        }
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "manual_ws recv frame failed: %s", esp_err_to_name(ret));
            return ret;
        }

        ret = mic_asr_doubao_handle_ws_frame(&frame);
        if (ret != ESP_OK) {
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
    manual_ws_close(&s_asr.ws);
    mic_asr_doubao_reset_session_state();
    mic_asr_doubao_log_heap("ASR stop after");
}
