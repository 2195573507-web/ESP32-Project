#include "mic_asr_doubao_parser.h"

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "miniz.h"

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
#if MIC_ASR_DOUBAO_PARSER_ENABLE_DEBUG_LOG
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
#endif

/**
 * @brief 把 serialization 转成人类可读名称。
 *
 * 调用方法：日志打印时调用。第一版主要期望 JSON，也允许把未知类型按原始文本
 * 安全打印，便于观察服务端实际返回。
 */
#if MIC_ASR_DOUBAO_PARSER_ENABLE_DEBUG_LOG
static const char *mic_asr_doubao_parser_serialization_name(uint8_t serialization)
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
 * @brief 打印二进制 payload 前 256 字节十六进制预览。
 *
 * 调用方法：每个服务端业务帧解析出 payload size 后调用。只看前
 * MIC_ASR_DOUBAO_PARSER_HEX_PREVIEW_BYTES 字节，既能确认 JSON/gzip/protobuf
 * 的开头特征，也避免把大响应或二进制内容刷满串口。
 */
static void mic_asr_doubao_parser_print_hex_preview(const uint8_t *payload, size_t payload_len)
{
#if MIC_ASR_DOUBAO_PARSER_ENABLE_DEBUG_LOG
    if (payload == NULL || payload_len == 0) {
        ESP_LOGI(TAG, "Doubao ASR payload hex: <empty>");
        return;
    }

    size_t preview_len = payload_len;
    if (preview_len > MIC_ASR_DOUBAO_PARSER_HEX_PREVIEW_BYTES) {
        preview_len = MIC_ASR_DOUBAO_PARSER_HEX_PREVIEW_BYTES;
    }

    char hex[MIC_ASR_DOUBAO_PARSER_HEX_PREVIEW_BYTES * 3 + 1] = {0};
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
             "Doubao ASR payload hex(%u/%u): %s%s",
             (unsigned int)preview_len,
             (unsigned int)payload_len,
             hex,
             payload_len > preview_len ? " ..." : "");
#else
    (void)payload;
    (void)payload_len;
#endif
}

/**
 * @brief 打印完整服务端帧的前缀十六进制。
 *
 * 调用方法：SERVER_ERROR 专用解析分支调用。错误帧字段不是普通 payload_size，
 * 因此需要保留 full frame 前缀，方便把 raw_header、error_code 和 error_message
 * 的实际字节对齐到服务端协议文档。
 */
static void mic_asr_doubao_parser_print_frame_hex_prefix(const uint8_t *frame, size_t frame_len)
{
#if MIC_ASR_DOUBAO_PARSER_ENABLE_DEBUG_LOG
    if (frame == NULL || frame_len == 0) {
        ESP_LOGI(TAG, "Doubao ASR frame hex prefix: <empty>");
        return;
    }

    size_t preview_len = frame_len;
    if (preview_len > MIC_ASR_DOUBAO_PARSER_HEX_PREVIEW_BYTES) {
        preview_len = MIC_ASR_DOUBAO_PARSER_HEX_PREVIEW_BYTES;
    }

    char hex[MIC_ASR_DOUBAO_PARSER_HEX_PREVIEW_BYTES * 3 + 1] = {0};
    size_t used = 0;
    for (size_t i = 0; i < preview_len && used + 3 < sizeof(hex); i++) {
        int written = snprintf(&hex[used],
                               sizeof(hex) - used,
                               "%02X%s",
                               (unsigned int)frame[i],
                               (i + 1 < preview_len) ? " " : "");
        if (written <= 0) {
            break;
        }
        used += (size_t)written;
    }

    ESP_LOGI(TAG,
             "Doubao ASR frame hex prefix(%u/%u): %s%s",
             (unsigned int)preview_len,
             (unsigned int)frame_len,
             hex,
             frame_len > preview_len ? " ..." : "");
#else
    (void)frame;
    (void)frame_len;
#endif
}

/**
 * @brief 把 compression 转成人类可读名称。
 *
 * 调用方法：日志打印时调用。gzip 第一版只提示不解压，避免把压缩数据当字符串。
 */
#if MIC_ASR_DOUBAO_PARSER_ENABLE_DEBUG_LOG
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
#endif

/**
 * @brief 安全打印未压缩 payload。
 *
 * 调用方法：确认 compression 不是 gzip 后调用。函数会在堆上申请
 * print_len + 1 字节，把 payload 复制进去并手动补 0，再用 %s 打印复制后的
 * C 字符串；绝不直接对原始 payload 使用 %s。
 */
static esp_err_t mic_asr_doubao_parser_print_payload(const uint8_t *payload, size_t payload_len)
{
#if MIC_ASR_DOUBAO_PARSER_ENABLE_DEBUG_LOG
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
#else
    (void)payload;
    (void)payload_len;
    return ESP_OK;
#endif
}

/**
 * @brief 安全打印 JSON 字符串字段。
 *
 * 调用方法：解析服务端 JSON payload 后打印 error/code/message/result/text。每个字段最多
 * 打印 MIC_ASR_DOUBAO_PARSER_JSON_FIELD_MAX_CHARS 个字符，避免长文本或异常堆栈刷屏。
 */
#if MIC_ASR_DOUBAO_PARSER_ENABLE_DEBUG_LOG
static void mic_asr_doubao_parser_log_json_string_field(const char *name, const cJSON *item)
{
    if (name == NULL || !cJSON_IsString(item) || item->valuestring == NULL) {
        return;
    }

    ESP_LOGI(TAG,
             "Doubao ASR JSON %s: %.*s%s",
             name,
             MIC_ASR_DOUBAO_PARSER_JSON_FIELD_MAX_CHARS,
             item->valuestring,
             strlen(item->valuestring) > MIC_ASR_DOUBAO_PARSER_JSON_FIELD_MAX_CHARS ? " ..." : "");
}
#endif

/**
 * @brief 解析并打印服务端 JSON payload 中的关键业务字段。
 *
 * 调用方法：payload serialization 为 JSON 且 compression 为 none/gzip 已得到明文后调用。
 * 这里重点打印错误码、错误信息、result.text 和顶层 text 字段，便于判断“本次未识别到
 * 文字”到底是服务端明确返回空 result，还是解析路径没有取到真实字段。
 */
static void mic_asr_doubao_parser_log_json_summary(const uint8_t *payload, size_t payload_len)
{
#if MIC_ASR_DOUBAO_PARSER_ENABLE_DEBUG_LOG
    if (payload == NULL || payload_len == 0) {
        ESP_LOGI(TAG, "Doubao ASR JSON summary: <empty>");
        return;
    }

    cJSON *root = cJSON_ParseWithLength((const char *)payload, payload_len);
    if (root == NULL) {
        ESP_LOGW(TAG, "Doubao ASR JSON parse failed");
        return;
    }

    const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(root, "message");
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    const cJSON *duration = cJSON_GetObjectItemCaseSensitive(root, "duration");
    const cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    const cJSON *top_text = cJSON_GetObjectItemCaseSensitive(root, "text");

    if (cJSON_IsNumber(code)) {
        ESP_LOGI(TAG, "Doubao ASR JSON code: %d", code->valueint);
    } else if (cJSON_IsString(code)) {
        mic_asr_doubao_parser_log_json_string_field("code", code);
    }
    mic_asr_doubao_parser_log_json_string_field("message", message);
    mic_asr_doubao_parser_log_json_string_field("error", error);
    mic_asr_doubao_parser_log_json_string_field("text", top_text);
    if (cJSON_IsNumber(duration)) {
        ESP_LOGI(TAG, "Doubao ASR JSON duration: %d", duration->valueint);
    } else if (cJSON_IsString(duration)) {
        mic_asr_doubao_parser_log_json_string_field("duration", duration);
    }

    if (cJSON_IsObject(result)) {
        const cJSON *result_text = cJSON_GetObjectItemCaseSensitive(result, "text");
        const cJSON *result_duration = cJSON_GetObjectItemCaseSensitive(result, "duration");
        mic_asr_doubao_parser_log_json_string_field("result.text", result_text);
        if (cJSON_IsNumber(result_duration)) {
            ESP_LOGI(TAG, "Doubao ASR JSON result.duration: %d", result_duration->valueint);
        } else if (cJSON_IsString(result_duration)) {
            mic_asr_doubao_parser_log_json_string_field("result.duration", result_duration);
        }
        if (cJSON_IsString(result_text) &&
            result_text->valuestring != NULL &&
            result_text->valuestring[0] == '\0') {
            ESP_LOGI(TAG, "Doubao ASR JSON result.text is empty");
        }
    } else if (result != NULL) {
        char *result_text = cJSON_PrintUnformatted(result);
        if (result_text != NULL) {
            ESP_LOGI(TAG,
                     "Doubao ASR JSON result: %.*s%s",
                     MIC_ASR_DOUBAO_PARSER_JSON_FIELD_MAX_CHARS,
                     result_text,
                     strlen(result_text) > MIC_ASR_DOUBAO_PARSER_JSON_FIELD_MAX_CHARS ? " ..." : "");
            cJSON_free(result_text);
        }
    }

    cJSON_Delete(root);
#else
    (void)payload;
    (void)payload_len;
#endif
}

/**
 * @brief 打印 protobuf payload 的有限摘要。
 *
 * 调用方法：服务端声明 serialization=protobuf 时调用。固件当前没有豆包 protobuf
 * 描述文件，因此不做字段级解码，只打印长度和可读 ASCII 片段，帮助确认是否误把
 * protobuf 当 JSON 解析导致空结果。
 */
static void mic_asr_doubao_parser_log_protobuf_summary(const uint8_t *payload, size_t payload_len)
{
#if MIC_ASR_DOUBAO_PARSER_ENABLE_DEBUG_LOG
    if (payload == NULL || payload_len == 0) {
        ESP_LOGI(TAG, "Doubao ASR protobuf summary: <empty>");
        return;
    }

    char ascii[129] = {0};
    size_t used = 0;
    for (size_t i = 0; i < payload_len && used + 1 < sizeof(ascii); i++) {
        uint8_t ch = payload[i];
        if (ch >= 0x20 && ch <= 0x7E) {
            ascii[used++] = (char)ch;
        }
    }

    ESP_LOGI(TAG,
             "Doubao ASR protobuf payload: size=%u ascii_hint=%s",
             (unsigned int)payload_len,
             used > 0 ? ascii : "<none>");
#else
    (void)payload;
    (void)payload_len;
#endif
}

/**
 * @brief 按当前 serialization 解析并打印 payload 摘要。
 *
 * 调用方法：原始 payload 未压缩，或 gzip 解压成功后调用。JSON 会打印 code、
 * message、error、text、result.text；protobuf 没有描述文件时只打印有限 ASCII 线索。
 */
static void mic_asr_doubao_parser_log_payload_summary(uint8_t serialization,
                                                      const uint8_t *payload,
                                                      size_t payload_len)
{
    if (serialization == MIC_ASR_DOUBAO_PARSER_SERIAL_JSON) {
        mic_asr_doubao_parser_log_json_summary(payload, payload_len);
    } else if (serialization == MIC_ASR_DOUBAO_PARSER_SERIAL_PROTOBUF) {
        mic_asr_doubao_parser_log_protobuf_summary(payload, payload_len);
    }
}

/**
 * @brief 从 gzip 包中定位真正的 deflate 数据段。
 *
 * 调用方法：服务端 compression 标记为 gzip 且 payload 以 1F 8B 开头时调用。gzip
 * 外层包含 10 字节基础头、可选 extra/name/comment/header-crc，以及最后 8 字节
 * CRC32/ISIZE 尾部；miniz 的 tinfl 解 raw deflate 时不能直接吃这些外壳，所以需要
 * 先把真正的 deflate 起点和长度找出来。
 *
 * @param payload gzip payload 指针。
 * @param payload_len gzip payload 字节数。
 * @param out_deflate 输出 deflate 数据起点。
 * @param out_deflate_len 输出 deflate 数据长度。
 * @return gzip 头合法且能定位 deflate 数据返回 true，否则返回 false。
 */
#if MIC_ASR_DOUBAO_PARSER_ENABLE_DEBUG_LOG
static bool mic_asr_doubao_parser_locate_gzip_deflate(const uint8_t *payload,
                                                      size_t payload_len,
                                                      const uint8_t **out_deflate,
                                                      size_t *out_deflate_len)
{
    if (payload == NULL || out_deflate == NULL || out_deflate_len == NULL || payload_len < 18) {
        return false;
    }
    if (payload[0] != 0x1F || payload[1] != 0x8B || payload[2] != 0x08) {
        return false;
    }

    enum {
        GZIP_FLAG_FTEXT = 0x01,
        GZIP_FLAG_FHCRC = 0x02,
        GZIP_FLAG_FEXTRA = 0x04,
        GZIP_FLAG_FNAME = 0x08,
        GZIP_FLAG_FCOMMENT = 0x10,
        GZIP_FLAG_RESERVED = 0xE0,
    };

    uint8_t gzip_flags = payload[3];
    if ((gzip_flags & GZIP_FLAG_RESERVED) != 0) {
        return false;
    }

    size_t cursor = 10;
    (void)GZIP_FLAG_FTEXT;
    if ((gzip_flags & GZIP_FLAG_FEXTRA) != 0) {
        if (payload_len - cursor < 2) {
            return false;
        }
        size_t extra_len = (size_t)payload[cursor] | ((size_t)payload[cursor + 1] << 8);
        cursor += 2;
        if (payload_len - cursor < extra_len) {
            return false;
        }
        cursor += extra_len;
    }

    if ((gzip_flags & GZIP_FLAG_FNAME) != 0) {
        while (cursor < payload_len && payload[cursor] != 0) {
            cursor++;
        }
        if (cursor >= payload_len) {
            return false;
        }
        cursor++;
    }

    if ((gzip_flags & GZIP_FLAG_FCOMMENT) != 0) {
        while (cursor < payload_len && payload[cursor] != 0) {
            cursor++;
        }
        if (cursor >= payload_len) {
            return false;
        }
        cursor++;
    }

    if ((gzip_flags & GZIP_FLAG_FHCRC) != 0) {
        if (payload_len - cursor < 2) {
            return false;
        }
        cursor += 2;
    }

    if (payload_len <= cursor + 8) {
        return false;
    }

    *out_deflate = &payload[cursor];
    *out_deflate_len = payload_len - cursor - 8;
    return true;
}
#endif

/**
 * @brief 尝试解压 gzip/zlib payload 并打印解压后的业务摘要。
 *
 * 调用方法：服务端声明 compression=gzip 时调用。函数使用 ESP-ROM miniz 的
 * tinfl_decompress_mem_to_mem()，只分配 MIC_ASR_DOUBAO_PARSER_GZIP_MAX_OUTPUT_BYTES
 * 固定上限缓冲；若服务端 payload 超过上限或不是 zlib/gzip 兼容流，就只保留 hex
 * 与明确失败原因，不影响 ASR 会话状态机。
 */
static void mic_asr_doubao_parser_log_gzip_summary(uint8_t serialization,
                                                   const uint8_t *payload,
                                                   size_t payload_len)
{
#if MIC_ASR_DOUBAO_PARSER_ENABLE_DEBUG_LOG
    bool looks_gzip = payload != NULL &&
                      payload_len >= 2 &&
                      payload[0] == 0x1F &&
                      payload[1] == 0x8B;
    ESP_LOGW(TAG,
             "Doubao ASR gzip payload: size=%u magic=%s",
             (unsigned int)payload_len,
             looks_gzip ? "gzip" : "not-gzip");

    if (payload == NULL || payload_len == 0) {
        return;
    }

    uint8_t *decoded = (uint8_t *)malloc(MIC_ASR_DOUBAO_PARSER_GZIP_MAX_OUTPUT_BYTES);
    if (decoded == NULL) {
        ESP_LOGW(TAG, "Doubao ASR gzip 解压缓冲分配失败");
        return;
    }

    const uint8_t *compressed_data = payload;
    size_t compressed_len = payload_len;
    int flags = TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF;
    if (looks_gzip) {
        if (!mic_asr_doubao_parser_locate_gzip_deflate(payload,
                                                       payload_len,
                                                       &compressed_data,
                                                       &compressed_len)) {
            ESP_LOGW(TAG, "Doubao ASR gzip 头解析失败，无法定位 deflate 数据段");
            free(decoded);
            return;
        }
    } else {
        flags |= TINFL_FLAG_PARSE_ZLIB_HEADER;
    }

    size_t decoded_len = tinfl_decompress_mem_to_mem(decoded,
                                                     MIC_ASR_DOUBAO_PARSER_GZIP_MAX_OUTPUT_BYTES,
                                                     compressed_data,
                                                     compressed_len,
                                                     flags);
    if (decoded_len == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
        ESP_LOGW(TAG,
                 "Doubao ASR gzip 解压失败或输出超过 %u 字节",
                 (unsigned int)MIC_ASR_DOUBAO_PARSER_GZIP_MAX_OUTPUT_BYTES);
        free(decoded);
        return;
    }

    ESP_LOGI(TAG,
             "Doubao ASR gzip decoded: %u bytes",
             (unsigned int)decoded_len);
    mic_asr_doubao_parser_print_hex_preview(decoded, decoded_len);
    mic_asr_doubao_parser_log_payload_summary(serialization, decoded, decoded_len);
    free(decoded);
#else
    (void)serialization;
    (void)payload;
    (void)payload_len;
#endif
}

/**
 * @brief 解析豆包 SERVER_ERROR 帧。
 *
 * 调用方法：mic_asr_doubao_parser_parse_one() 识别 message_type=0xF 后立即调用。
 * SERVER_ERROR 的 body 不是普通 payload_size + payload，而是：
 *   4 字节协议头（长度由 header_size 决定）
 *   4 字节 error_code（big-endian）
 *   4 字节 error_message_size（big-endian）
 *   error_message 原文字节
 * 因此这里不能把 error_code 误当成 payload_size，否则会得到类似 45000000 的假长度。
 */
static esp_err_t mic_asr_doubao_parser_parse_server_error(const uint8_t *data,
                                                          size_t len,
                                                          size_t header_bytes,
                                                          uint8_t version,
                                                          uint8_t flags,
                                                          uint8_t serialization,
                                                          uint8_t compression,
                                                          size_t *consumed)
{
    if (data == NULL || consumed == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *consumed = 0;

    size_t cursor = header_bytes;
    size_t min_error_bytes = header_bytes +
                             MIC_ASR_DOUBAO_PARSER_ERROR_CODE_BYTES +
                             MIC_ASR_DOUBAO_PARSER_ERROR_SIZE_BYTES;
    if (len < min_error_bytes) {
        ESP_LOGE(TAG,
                 "Doubao ASR SERVER_ERROR 帧格式错: len=%u header=%u need=%u",
                 (unsigned int)len,
                 (unsigned int)header_bytes,
                 (unsigned int)min_error_bytes);
        mic_asr_doubao_parser_print_frame_hex_prefix(data, len);
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t error_code = mic_asr_doubao_parser_read_u32_be(&data[cursor]);
    cursor += MIC_ASR_DOUBAO_PARSER_ERROR_CODE_BYTES;
    uint32_t error_message_size = mic_asr_doubao_parser_read_u32_be(&data[cursor]);
    cursor += MIC_ASR_DOUBAO_PARSER_ERROR_SIZE_BYTES;

    if ((size_t)error_message_size > len - cursor) {
        ESP_LOGE(TAG,
                 "Doubao ASR SERVER_ERROR message 越界: error_code=%" PRIu32 ", message_size=%" PRIu32 ", available=%u",
                 error_code,
                 error_message_size,
                 (unsigned int)(len - cursor));
        mic_asr_doubao_parser_print_frame_hex_prefix(data, len);
        return ESP_ERR_INVALID_SIZE;
    }

    const uint8_t *message = error_message_size > 0 ? &data[cursor] : NULL;
    mic_asr_doubao_parser_print_frame_hex_prefix(data, cursor + (size_t)error_message_size);

    ESP_LOGE(TAG,
             "Doubao ASR SERVER_ERROR: error_code=%" PRIu32 ", error_message_size=%" PRIu32,
             error_code,
             error_message_size);
#if MIC_ASR_DOUBAO_PARSER_ENABLE_DEBUG_LOG
    ESP_LOGE(TAG,
             "Doubao ASR SERVER_ERROR detail: version=%u header=%u flags=0x%X serialization=0x%X(%s) compression=0x%X(%s)",
             (unsigned int)version,
             (unsigned int)header_bytes,
             (unsigned int)flags,
             (unsigned int)serialization,
             mic_asr_doubao_parser_serialization_name(serialization),
             (unsigned int)compression,
             mic_asr_doubao_parser_compression_name(compression));
#else
    (void)version;
    (void)flags;
    (void)serialization;
    (void)compression;
#endif

    if (error_message_size > 0) {
        size_t print_len = (size_t)error_message_size;
        if (print_len > MIC_ASR_DOUBAO_PARSER_MAX_PRINT_BYTES) {
            print_len = MIC_ASR_DOUBAO_PARSER_MAX_PRINT_BYTES;
        }
        char *text = (char *)malloc(print_len + 1U);
        if (text == NULL) {
            ESP_LOGE(TAG, "Doubao ASR SERVER_ERROR 文本缓冲分配失败: len=%u", (unsigned int)print_len);
            return ESP_ERR_NO_MEM;
        }
        memcpy(text, message, print_len);
        text[print_len] = '\0';
        ESP_LOGE(TAG,
                 "Doubao ASR SERVER_ERROR message(%u/%" PRIu32 "): %s",
                 (unsigned int)print_len,
                 error_message_size,
                 text);
        if (print_len < (size_t)error_message_size) {
            ESP_LOGW(TAG,
                     "Doubao ASR SERVER_ERROR message 过长，仅打印前 %u 字节",
                     (unsigned int)MIC_ASR_DOUBAO_PARSER_MAX_PRINT_BYTES);
        }
        free(text);
    } else {
        ESP_LOGE(TAG, "Doubao ASR SERVER_ERROR message: <empty>");
    }

    *consumed = cursor + (size_t)error_message_size;
    return ESP_FAIL;
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

    if (message_type == MIC_ASR_DOUBAO_PARSER_MSG_SERVER_ERROR) {
        return mic_asr_doubao_parser_parse_server_error(data,
                                                        len,
                                                        header_bytes,
                                                        version,
                                                        flags,
                                                        serialization,
                                                        compression,
                                                        consumed);
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
    size_t payload_size = (size_t)payload_size_u32;
    if (payload_size > len - cursor) {
        ESP_LOGE(TAG,
                 "Doubao ASR parser payload 越界: payload=%" PRIu32 ", available=%u",
                 payload_size_u32,
                 (unsigned int)(len - cursor));
        return ESP_ERR_INVALID_SIZE;
    }

#if MIC_ASR_DOUBAO_PARSER_ENABLE_DEBUG_LOG
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
#else
    (void)version;
    (void)header_bytes;
    (void)has_sequence;
    (void)sequence;
    (void)serialization;
    (void)payload_size_u32;
#endif

    const uint8_t *payload = payload_size > 0 ? &data[cursor] : NULL;
    *consumed = cursor + payload_size;
    mic_asr_doubao_parser_print_hex_preview(payload, payload_size);

    if (message_type != MIC_ASR_DOUBAO_PARSER_MSG_SERVER_ACK &&
        message_type != MIC_ASR_DOUBAO_PARSER_MSG_FULL_RESPONSE &&
        message_type != MIC_ASR_DOUBAO_PARSER_MSG_SERVER_ERROR) {
        ESP_LOGW(TAG, "Doubao ASR parser 收到未知 message type=0x%X", (unsigned int)message_type);
    }

    if (compression == MIC_ASR_DOUBAO_PARSER_COMPRESS_GZIP) {
        mic_asr_doubao_parser_log_gzip_summary(serialization, payload, payload_size);
        if (message_type == MIC_ASR_DOUBAO_PARSER_MSG_SERVER_ERROR) {
            ESP_LOGE(TAG, "Doubao ASR parser 收到 gzip SERVER_ERROR");
            return ESP_FAIL;
        }
        return ESP_OK;
    }
    if (compression != MIC_ASR_DOUBAO_PARSER_COMPRESS_NONE) {
        ESP_LOGW(TAG,
                 "Doubao ASR parser 收到未知 compression=0x%X，按未压缩安全打印",
                 (unsigned int)compression);
    }

    esp_err_t ret = mic_asr_doubao_parser_print_payload(payload, payload_size);
    mic_asr_doubao_parser_log_payload_summary(serialization, payload, payload_size);
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
