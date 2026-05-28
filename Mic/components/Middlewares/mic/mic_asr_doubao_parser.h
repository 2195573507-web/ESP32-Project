#ifndef MIC_ASR_DOUBAO_PARSER_H
#define MIC_ASR_DOUBAO_PARSER_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

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

/* compression：第一版不做 gzip 解压，只识别并提示。 */
#define MIC_ASR_DOUBAO_PARSER_COMPRESS_NONE      0x0  // 未压缩。
#define MIC_ASR_DOUBAO_PARSER_COMPRESS_GZIP      0x1  // gzip 压缩，暂不解压。

/* 串口打印保护：payload 太长时只复制和打印前面一段，避免占用过多堆内存。 */
#define MIC_ASR_DOUBAO_PARSER_MAX_PRINT_BYTES    2048 // 单次安全打印的最大 payload 字节数。

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
 * @return 解析成功返回 ESP_OK；长度越界、参数无效、gzip 暂不支持或服务端错误返回错误码。
 */
esp_err_t mic_asr_doubao_parser_parse(const uint8_t *data, size_t len);

#endif // MIC_ASR_DOUBAO_PARSER_H
