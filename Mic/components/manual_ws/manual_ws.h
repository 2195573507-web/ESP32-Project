#ifndef MANUAL_WS_H
#define MANUAL_WS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_tls.h"

/**
 * @file manual_ws.h
 * @brief 单连接、阻塞式、基于 esp_tls 的轻量 WebSocket 协议层。
 *
 * 本模块只处理 RFC6455 WebSocket 协议：TLS 建连、HTTP Upgrade 握手、
 * client frame 自动 mask、server frame 解析、ping/pong/close 控制帧处理。
 * 业务 Header、业务二进制 payload 和上层协议解析都由调用者负责。
 */

/* 缓冲与超时：调试握手头过大可上调 HANDSHAKE；调试 read 边界可调小 IO。 */
#define MANUAL_WS_HANDSHAKE_BUF_SIZE      4096                                  // HTTP Upgrade 响应头最大缓存，必要时调到 8192。
#define MANUAL_WS_IO_BUF_SIZE             512                                   // 握手分片读取临时缓冲。
#define MANUAL_WS_MAX_PAYLOAD_SIZE        4096                                  // 单帧 payload 上限，过大返回 ESP_ERR_INVALID_SIZE。
#define MANUAL_WS_DEFAULT_TIMEOUT_MS      15000                                 // TLS 建连和默认读写超时。
#define MANUAL_WS_ENABLE_DEBUG_LOG        0                                     // 详细日志开关，调协议时改 1。

/* WebSocket 握手常量：Sec-WebSocket-Key 为 16 字节随机数的 24 字符 Base64。 */
#define MANUAL_WS_SEC_KEY_BASE64_LEN      24                                    // 不含结尾 NUL。
#define MANUAL_WS_SEC_KEY_BASE64_BUF_LEN  (MANUAL_WS_SEC_KEY_BASE64_LEN + 1)    // 含结尾 NUL。
#define MANUAL_WS_GUID                    "258EAFA5-E914-47DA-95CA-C5AB0DC85B11" // Accept 计算固定 GUID。

/* RFC6455 opcode，放在公共头里方便上层判断 manual_ws_recv_frame() 返回的帧类型。 */
#define MANUAL_WS_OPCODE_CONTINUATION     0x0
#define MANUAL_WS_OPCODE_TEXT             0x1
#define MANUAL_WS_OPCODE_BINARY           0x2
#define MANUAL_WS_OPCODE_CLOSE            0x8
#define MANUAL_WS_OPCODE_PING             0x9
#define MANUAL_WS_OPCODE_PONG             0xA

/**
 * @brief WebSocket 连接参数。
 *
 * host/path/port 由业务层传入，manual_ws 不写死任何服务商域名、路径或鉴权 Header。
 * extra_headers 可以包含多行 "Key: Value\r\n"，会追加到标准 Upgrade Header 后面。
 */
typedef struct {
    const char *host;          // 目标主机名，例如 example.com，不能为空。
    const char *path;          // HTTP request target，例如 /ws 或 /api/ws?x=1，不能为空。
    int port;                  // TLS 端口，wss 默认 443；传 0 时使用 443。
    int timeout_ms;            // TLS/网络超时；传 0 时使用 MANUAL_WS_DEFAULT_TIMEOUT_MS。
    const char *extra_headers; // 业务自定义 Header 块，可为 NULL；不要包含标准 WebSocket Header。
} manual_ws_config_t;

/**
 * @brief WebSocket 客户端状态。
 *
 * 当前第一版只支持一个结构体对应一条阻塞式连接，不创建额外 task，不做自动重连。
 */
typedef struct {
    uint32_t magic;            // 内部初始化标记；调用者不需要读写。
    esp_tls_t *tls;            // esp_tls 连接句柄，由 manual_ws_connect()/close() 管理。
    bool connected;            // HTTP 101 握手完成后置 true；close/destroy 后置 false。
    char host[128];            // 保存当前连接 host，用于日志和证书 common_name。
    char path[192];            // 保存当前连接 path，用于日志和握手请求。
    int port;                  // 当前连接端口。
    int timeout_ms;            // 当前连接使用的 esp_tls timeout_ms。
} manual_ws_client_t;

/**
 * @brief manual_ws_recv_frame() 返回的帧信息。
 *
 * payload 指向调用者传入的 payload_buf；下一次 recv 会覆盖同一缓冲区。
 */
typedef struct {
    bool fin;                  // FIN 位，true 表示当前消息结束。
    uint8_t opcode;            // RFC6455 opcode，例如 MANUAL_WS_OPCODE_BINARY。
    bool masked;               // 服务端帧是否带 mask；正常服务端帧应为 false。
    size_t payload_len;        // payload 实际长度。
    uint8_t *payload;          // payload_buf 指针；payload_len 为 0 时可为 NULL。
} manual_ws_frame_t;

esp_err_t manual_ws_connect(manual_ws_client_t *client, const manual_ws_config_t *config);
esp_err_t manual_ws_send_binary(manual_ws_client_t *client, const uint8_t *data, size_t len);
esp_err_t manual_ws_send_text(manual_ws_client_t *client, const char *text);
esp_err_t manual_ws_send_close(manual_ws_client_t *client);
esp_err_t manual_ws_recv_frame(manual_ws_client_t *client,
                               manual_ws_frame_t *frame,
                               uint8_t *payload_buf,
                               size_t payload_buf_size,
                               int timeout_ms);
void manual_ws_close(manual_ws_client_t *client);

#endif // MANUAL_WS_H
