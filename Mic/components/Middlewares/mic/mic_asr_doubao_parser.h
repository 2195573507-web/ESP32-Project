#ifndef MIC_ASR_DOUBAO_PARSER_H
#define MIC_ASR_DOUBAO_PARSER_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "app_debug_config.h"

/**
 * @file mic_asr_doubao_parser.h
 * @brief 豆包 ASR WebSocket 服务端二进制协议解析模块。
 *
 * 本模块只解析豆包 ASR 服务端返回的业务二进制协议，不负责 WebSocket/TLS，
 * 不依赖 mic_adc、VAD 或 record_buffer。manual_ws 仍只负责通用 WebSocket 收发。
 */

/* 基础帧结构：4 字节协议头 + 可选 sequence + 4 字节大端 payload size + payload。 */
#define MIC_ASR_DOUBAO_PARSER_HEADER_BYTES       4    // 协议头固定 4 字节。
#define MIC_ASR_DOUBAO_PARSER_HEADER_WORD_BYTES  4    // header size 单位，1 word = 4 字节。
#define MIC_ASR_DOUBAO_PARSER_PAYLOAD_SIZE_BYTES 4    // payload size 固定 4 字节大端序。
#define MIC_ASR_DOUBAO_PARSER_SEQUENCE_BYTES     4    // flags 带 sequence 时的序列号长度。
#define MIC_ASR_DOUBAO_PARSER_ERROR_CODE_BYTES   4    // SERVER_ERROR error_code 固定 4 字节大端序。
#define MIC_ASR_DOUBAO_PARSER_ERROR_SIZE_BYTES   4    // SERVER_ERROR error_message_size 固定 4 字节大端序。
#define MIC_ASR_DOUBAO_PARSER_MIN_FRAME_BYTES    8    // 无 sequence 时最小帧长度。

/* 第 1 字节：protocol version 和 header size。 */
#define MIC_ASR_DOUBAO_PARSER_VERSION_MASK       0xF0 // 高 4 bit 是协议版本。
#define MIC_ASR_DOUBAO_PARSER_VERSION_SHIFT      4    // 协议版本右移位数。
#define MIC_ASR_DOUBAO_PARSER_HEADER_SIZE_MASK   0x0F // 低 4 bit 是 header word 数。

/* 第 2 字节：message type 和 flags。 */
#define MIC_ASR_DOUBAO_PARSER_MSG_TYPE_MASK      0xF0 // 高 4 bit 是 message type。
#define MIC_ASR_DOUBAO_PARSER_MSG_TYPE_SHIFT     4    // message type 右移位数。
#define MIC_ASR_DOUBAO_PARSER_FLAGS_MASK         0x0F // 低 4 bit 是 flags。

/* 第 3 字节：serialization 和 compression。 */
#define MIC_ASR_DOUBAO_PARSER_SERIAL_MASK        0xF0 // 高 4 bit 是序列化类型。
#define MIC_ASR_DOUBAO_PARSER_SERIAL_SHIFT       4    // serialization 右移位数。
#define MIC_ASR_DOUBAO_PARSER_COMPRESS_MASK      0x0F // 低 4 bit 是压缩类型。

/* 服务端 message type：第一版只识别 ACK、完整响应和错误响应。 */
#define MIC_ASR_DOUBAO_PARSER_MSG_FULL_RESPONSE  0x9  // FULL_SERVER_RESPONSE。
#define MIC_ASR_DOUBAO_PARSER_MSG_SERVER_ACK     0xB  // SERVER_ACK。
#define MIC_ASR_DOUBAO_PARSER_MSG_SERVER_ERROR   0xF  // SERVER_ERROR。

/* flags：当前只用来判断是否存在 4 字节 sequence 字段。 */
#define MIC_ASR_DOUBAO_PARSER_FLAG_NO_SEQUENCE   0x0  // 无 sequence。
#define MIC_ASR_DOUBAO_PARSER_FLAG_POS_SEQUENCE  0x1  // 带正向 sequence。
#define MIC_ASR_DOUBAO_PARSER_FLAG_LAST_NO_SEQ   0x2  // 末包标记，无 sequence。
#define MIC_ASR_DOUBAO_PARSER_FLAG_NEG_SEQUENCE  0x3  // 带负向 sequence。

/* serialization：用于日志说明 payload 格式。 */
#define MIC_ASR_DOUBAO_PARSER_SERIAL_NONE        0x0  // 无序列化。
#define MIC_ASR_DOUBAO_PARSER_SERIAL_JSON        0x1  // JSON payload。
#define MIC_ASR_DOUBAO_PARSER_SERIAL_PROTOBUF    0x2  // Protobuf payload；无 proto 描述时只能打印 wire 摘要和字符串片段。

/* compression：gzip 只在调试路径做 4KB 上限解压，不改变 ASR 会话和发送协议。 */
#define MIC_ASR_DOUBAO_PARSER_COMPRESS_NONE      0x0  // 未压缩。
#define MIC_ASR_DOUBAO_PARSER_COMPRESS_GZIP      0x1  // gzip 压缩。

/* 串口打印保护：payload 太长时只复制和打印前面一段，避免占用过多堆内存。 */
#define MIC_ASR_DOUBAO_PARSER_MAX_PRINT_BYTES       APP_DEBUG_ASR_PARSER_MAX_PRINT_BYTES       // 单次安全文本打印的最大 payload 字节数。
#define MIC_ASR_DOUBAO_PARSER_ENABLE_HEADER_LOG     APP_DEBUG_ASR_PROTOCOL_HEADER_LOG          // 服务端业务协议头字段日志。
#define MIC_ASR_DOUBAO_PARSER_ENABLE_PAYLOAD_TEXT_LOG (APP_DEBUG_ASR_PAYLOAD_TEXT_DUMP || APP_DEBUG_ASR_PAYLOAD_FULL_TEXT_DUMP) // payload 文本/JSON/protobuf 摘要。
#define MIC_ASR_DOUBAO_PARSER_ENABLE_PAYLOAD_HEX_LOG APP_DEBUG_ASR_PAYLOAD_HEX_DUMP            // payload 十六进制预览。
#define MIC_ASR_DOUBAO_PARSER_ENABLE_SERVER_ERROR_DETAIL_LOG APP_DEBUG_ASR_SERVER_ERROR_DETAIL_LOG // SERVER_ERROR 协议字段和帧前缀。
#define MIC_ASR_DOUBAO_PARSER_ENABLE_DEBUG_LOG      (MIC_ASR_DOUBAO_PARSER_ENABLE_HEADER_LOG || MIC_ASR_DOUBAO_PARSER_ENABLE_PAYLOAD_TEXT_LOG || MIC_ASR_DOUBAO_PARSER_ENABLE_PAYLOAD_HEX_LOG || MIC_ASR_DOUBAO_PARSER_ENABLE_SERVER_ERROR_DETAIL_LOG) // 兼容旧调试总开关。
#define MIC_ASR_DOUBAO_PARSER_HEX_PREVIEW_BYTES     APP_DEBUG_ASR_PAYLOAD_HEX_PREVIEW_BYTES    // 服务端业务 payload hex 预览字节数。
#define MIC_ASR_DOUBAO_PARSER_JSON_FIELD_MAX_CHARS  APP_DEBUG_ASR_PARSER_JSON_FIELD_MAX_CHARS  // JSON code/message/result/text 字段最多打印字符数。
#define MIC_ASR_DOUBAO_PARSER_GZIP_MAX_OUTPUT_BYTES APP_DEBUG_ASR_PARSER_GZIP_MAX_OUTPUT_BYTES // gzip 调试解压最多输出字节数。

/**
 * @brief 解析豆包 ASR 服务端 WebSocket binary payload。
 *
 * 调用方法：mic_asr_doubao.c 收到 manual_ws 的 binary frame，或重组完
 * continuation 后，把完整业务 message 的 data/len 传入本函数。函数会解析
 * 4 字节协议头、payload size、message type、flags、serialization、
 * compression，并安全复制非 gzip payload 后补 0 打印。
 *
 * @param data WebSocket binary payload 指针，不能为空。
 * @param len WebSocket binary payload 长度。
 * @return 解析成功返回 ESP_OK；长度越界、参数无效或服务端错误返回错误码。
 */
esp_err_t mic_asr_doubao_parser_parse(const uint8_t *data, size_t len);

#endif // MIC_ASR_DOUBAO_PARSER_H
