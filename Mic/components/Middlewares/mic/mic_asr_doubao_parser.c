#include "mic_asr_doubao_parser.h"

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "doubao_parser";

/**
 * @brief 从 4 字节大端序读取 uint32_t。
 *
 * 调用方法：解析 payload size 时调用。调用前必须确认 bytes 至少还有
 * MIC_ASR_DOUBAO_PARSER_PAYLOAD_SIZE_BYTES 字节，避免越界读取。
 */
static uint32_t mic_asr_doubao_parser_read_u32_be(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24) |
           ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) |
           (uint32_t)bytes[3];
}

/**
 * @brief 判断 flags 是否携带 sequence 字段。
 *
 * 调用方法：解析完 4 字节协议头后调用。豆包服务端 ACK/错误包可能带 sequence，
 * parser 第一版只负责跳过并打印，不把 sequence 交给上层状态机。
 */
static bool mic_asr_doubao_parser_has_sequence(uint8_t flags)
{
    return flags == MIC_ASR_DOUBAO_PARSER_FLAG_POS_SEQUENCE ||
           flags == MIC_ASR_DOUBAO_PARSER_FLAG_NEG_SEQUENCE;
}

/**
 * @brief 把 message type 转成人类可读名称。
 *
 * 调用方法：日志打印时调用，便于串口确认当前收到的是 SERVER_ACK、
 * FULL_SERVER_RESPONSE 还是 SERVER_ERROR。
 */
static const char *mic_asr_doubao_parser_message_type_name(uint8_t message_type)
{
    switch (message_type) {
    case MIC_ASR_DOUBAO_PARSER_MSG_SERVER_ACK:
        return "SERVER_ACK";
    case MIC_ASR_DOUBAO_PARSER_MSG_FULL_RESPONSE:
        return "FULL_SERVER_RESPONSE";
    case MIC_ASR_DOUBAO_PARSER_MSG_SERVER_ERROR:
        return "SERVER_ERROR";
    default:
        return "UNKNOWN";
    }
}

/**
 * @brief 把 serialization 转成人类可读名称。
 *
 * 调用方法：日志打印时调用。第一版主要期望 JSON，也允许把未知类型按原始文本
 * 安全打印，便于观察服务端实际返回。
 */
static const char *mic_asr_doubao_parser_serialization_name(uint8_t serialization)
{
    switch (serialization) {
    case MIC_ASR_DOUBAO_PARSER_SERIAL_NONE:
        return "none";
    case MIC_ASR_DOUBAO_PARSER_SERIAL_JSON:
        return "json";
    default:
        return "unknown";
    }
}

/**
 * @brief 把 compression 转成人类可读名称。
 *
 * 调用方法：日志打印时调用。gzip 第一版只提示不解压，避免把压缩数据当字符串。
 */
static const char *mic_asr_doubao_parser_compression_name(uint8_t compression)
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

/**
 * @brief 安全打印未压缩 payload。
 *
 * 调用方法：确认 compression 不是 gzip 后调用。函数会在堆上申请
 * print_len + 1 字节，把 payload 复制进去并手动补 0，再用 %s 打印复制后的
 * C 字符串；绝不直接对原始 payload 使用 %s。
 */
static esp_err_t mic_asr_doubao_parser_print_payload(const uint8_t *payload, size_t payload_len)
{
    if (payload_len == 0) {
        ESP_LOGI(TAG, "Doubao ASR payload: <empty>");
        return ESP_OK;
    }
    if (payload == NULL) {
        ESP_LOGE(TAG, "Doubao ASR parser 参数错误: payload 为空但长度非 0");
        return ESP_ERR_INVALID_ARG;
    }

    size_t print_len = payload_len;
    if (print_len > MIC_ASR_DOUBAO_PARSER_MAX_PRINT_BYTES) {
        print_len = MIC_ASR_DOUBAO_PARSER_MAX_PRINT_BYTES;
    }

    char *text = (char *)malloc(print_len + 1U);
    if (text == NULL) {
        ESP_LOGE(TAG, "Doubao ASR parser payload 打印缓冲分配失败: len=%u", (unsigned int)print_len);
        return ESP_ERR_NO_MEM;
    }

    memcpy(text, payload, print_len);
    text[print_len] = '\0';

    ESP_LOGI(TAG,
             "Doubao ASR payload text(%u/%u): %s",
             (unsigned int)print_len,
             (unsigned int)payload_len,
             text);
    if (print_len < payload_len) {
        ESP_LOGW(TAG,
                 "Doubao ASR payload 过长，仅打印前 %u 字节",
                 (unsigned int)MIC_ASR_DOUBAO_PARSER_MAX_PRINT_BYTES);
    }

    free(text);
    return ESP_OK;
}

/**
 * @brief 解析单个豆包 ASR 服务端协议帧。
 *
 * 调用方法：mic_asr_doubao_parser_parse() 在遍历 WebSocket binary payload 时调用。
 * 本函数按 4 字节协议头解析 version/header size/message type/flags/
 * serialization/compression，然后按 4 字节大端 payload size 定位 payload。
 */
static esp_err_t mic_asr_doubao_parser_parse_one(const uint8_t *data,
                                                 size_t len,
                                                 size_t *consumed)
{
    if (data == NULL || consumed == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *consumed = 0;

    if (len < MIC_ASR_DOUBAO_PARSER_MIN_FRAME_BYTES) {
        ESP_LOGE(TAG, "Doubao ASR parser 帧格式错: 长度不足 len=%u", (unsigned int)len);
        return ESP_ERR_INVALID_SIZE;
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

    if (header_bytes < MIC_ASR_DOUBAO_PARSER_HEADER_BYTES) {
        ESP_LOGE(TAG, "Doubao ASR parser 帧格式错: header size 无效=%u", (unsigned int)header_bytes);
        return ESP_ERR_INVALID_SIZE;
    }
    if (len < header_bytes + MIC_ASR_DOUBAO_PARSER_PAYLOAD_SIZE_BYTES) {
        ESP_LOGE(TAG,
                 "Doubao ASR parser 帧格式错: header/payload size 越界 len=%u header=%u",
                 (unsigned int)len,
                 (unsigned int)header_bytes);
        return ESP_ERR_INVALID_SIZE;
    }

    size_t cursor = header_bytes;
    int32_t sequence = 0;
    bool has_sequence = mic_asr_doubao_parser_has_sequence(flags);
    if (has_sequence) {
        if (len < cursor + MIC_ASR_DOUBAO_PARSER_SEQUENCE_BYTES +
                  MIC_ASR_DOUBAO_PARSER_PAYLOAD_SIZE_BYTES) {
            ESP_LOGE(TAG, "Doubao ASR parser 帧格式错: sequence 字段越界");
            return ESP_ERR_INVALID_SIZE;
        }
        sequence = (int32_t)mic_asr_doubao_parser_read_u32_be(&data[cursor]);
        cursor += MIC_ASR_DOUBAO_PARSER_SEQUENCE_BYTES;
    }

    uint32_t payload_size_u32 = mic_asr_doubao_parser_read_u32_be(&data[cursor]);
    cursor += MIC_ASR_DOUBAO_PARSER_PAYLOAD_SIZE_BYTES;
    if ((uint64_t)payload_size_u32 > (uint64_t)SIZE_MAX) {
        ESP_LOGE(TAG, "Doubao ASR parser payload 长度超过 size_t: payload=%" PRIu32, payload_size_u32);
        return ESP_ERR_INVALID_SIZE;
    }
    size_t payload_size = (size_t)payload_size_u32;
    if (payload_size > len - cursor) {
        ESP_LOGE(TAG,
                 "Doubao ASR parser payload 越界: payload=%" PRIu32 ", available=%u",
                 payload_size_u32,
                 (unsigned int)(len - cursor));
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG,
             "Doubao ASR frame: version=%u header=%u type=0x%X(%s) flags=0x%X sequence=%s%d serialization=0x%X(%s) compression=0x%X(%s) payload=%" PRIu32,
             (unsigned int)version,
             (unsigned int)header_bytes,
             (unsigned int)message_type,
             mic_asr_doubao_parser_message_type_name(message_type),
             (unsigned int)flags,
             has_sequence ? "" : "none/",
             has_sequence ? (int)sequence : 0,
             (unsigned int)serialization,
             mic_asr_doubao_parser_serialization_name(serialization),
             (unsigned int)compression,
             mic_asr_doubao_parser_compression_name(compression),
             payload_size_u32);

    const uint8_t *payload = payload_size > 0 ? &data[cursor] : NULL;
    *consumed = cursor + payload_size;

    if (message_type != MIC_ASR_DOUBAO_PARSER_MSG_SERVER_ACK &&
        message_type != MIC_ASR_DOUBAO_PARSER_MSG_FULL_RESPONSE &&
        message_type != MIC_ASR_DOUBAO_PARSER_MSG_SERVER_ERROR) {
        ESP_LOGW(TAG, "Doubao ASR parser 收到未知 message type=0x%X", (unsigned int)message_type);
    }

    if (compression == MIC_ASR_DOUBAO_PARSER_COMPRESS_GZIP) {
        ESP_LOGW(TAG, "Doubao ASR parser 暂不支持 gzip payload 解压，已跳过打印");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (compression != MIC_ASR_DOUBAO_PARSER_COMPRESS_NONE) {
        ESP_LOGW(TAG,
                 "Doubao ASR parser 收到未知 compression=0x%X，按未压缩安全打印",
                 (unsigned int)compression);
    }

    esp_err_t ret = mic_asr_doubao_parser_print_payload(payload, payload_size);
    if (message_type == MIC_ASR_DOUBAO_PARSER_MSG_SERVER_ERROR) {
        ESP_LOGE(TAG, "Doubao ASR parser 收到 SERVER_ERROR");
        return ret == ESP_OK ? ESP_FAIL : ret;
    }

    return ret;
}

esp_err_t mic_asr_doubao_parser_parse(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        ESP_LOGE(TAG, "Doubao ASR parser 参数错误: data=%p len=%u", data, (unsigned int)len);
        return ESP_ERR_INVALID_ARG;
    }

    size_t offset = 0;
    esp_err_t final_ret = ESP_OK;
    while (offset < len) {
        size_t consumed = 0;
        esp_err_t ret = mic_asr_doubao_parser_parse_one(&data[offset], len - offset, &consumed);
        if (ret != ESP_OK && final_ret == ESP_OK) {
            final_ret = ret;
        }
        if (consumed == 0) {
            return ret;
        }
        offset += consumed;
    }

    return final_ret;
}
