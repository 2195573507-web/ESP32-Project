#include "manual_ws.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/time.h>

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_log_write.h"
#include "esp_random.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha1.h"
#include "lwip/sockets.h"

static const char *TAG = "manual_ws";

enum {
    MANUAL_WS_TLS_READ_WANT_READ = -0x6900,
    MANUAL_WS_TLS_READ_TIMEOUT_WARN_EVERY = 20,
};

/**
 * @brief manual_ws 详细调试日志入口。
 *
 * 调用方法：仅在 MANUAL_WS_ENABLE_DEBUG_LOG 为 1 时真正输出；默认关闭，避免
 * ASR 音频发送/接收循环反复进入 vfprintf。
 */
static void manual_ws_log_debug(const char *format, ...)
{
    if (!MANUAL_WS_ENABLE_DEBUG_LOG) {
        return;
    }

    va_list args;
    va_start(args, format);
    esp_log_writev(ESP_LOG_INFO, TAG, format, args);
    va_end(args);
}

enum {
    MANUAL_WS_CLIENT_MAGIC = 0x4D575331U,
    MANUAL_WS_SEC_KEY_RAW_BYTES = 16,
    MANUAL_WS_ACCEPT_SHA1_BYTES = 20,
    MANUAL_WS_ACCEPT_BASE64_LEN = 28,
    MANUAL_WS_MASK_KEY_BYTES = 4,
    MANUAL_WS_BASE_HEADER_BYTES = 2,
    MANUAL_WS_EXT16_BYTES = 2,
    MANUAL_WS_EXT64_BYTES = 8,
};

/**
 * @brief 把 opcode 转成日志可读名称。
 *
 * 调用方法：发送和接收帧日志都调用它，只用于调试输出，不参与协议判断。
 */
static const char *manual_ws_opcode_name(uint8_t opcode)
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
 * @brief 判断累计 HTTP 响应头是否已经结束。
 *
 * 调用方法：握手读取循环每追加一段 TLS 数据后调用。由于 "\r\n\r\n" 可能跨越
 * 两次 esp_tls_conn_read()，这里扫描完整累计 buffer，而不是只看本次分片。
 */
static bool manual_ws_http_header_complete(const char *text, size_t text_len)
{
    if (text == NULL || text_len < 4) {
        return false;
    }

    for (size_t i = 0; i + 3 < text_len; i++) {
        if (text[i] == '\r' &&
            text[i + 1] == '\n' &&
            text[i + 2] == '\r' &&
            text[i + 3] == '\n') {
            return true;
        }
    }
    return false;
}

/**
 * @brief ASCII 大小写不敏感地判断 haystack 是否包含 needle。
 *
 * 调用方法：HTTP Header 字段名大小写不敏感，检查 Sec-WebSocket-Accept 时用它避免
 * 服务端大小写变化导致误判。
 */
static bool manual_ws_contains_case_insensitive(const char *haystack, const char *needle)
{
    if (haystack == NULL || needle == NULL || needle[0] == '\0') {
        return false;
    }

    size_t needle_len = strlen(needle);
    for (const char *cursor = haystack; *cursor != '\0'; cursor++) {
        size_t i = 0;
        while (i < needle_len && cursor[i] != '\0') {
            char a = cursor[i];
            char b = needle[i];
            if (a >= 'A' && a <= 'Z') {
                a = (char)(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z') {
                b = (char)(b - 'A' + 'a');
            }
            if (a != b) {
                break;
            }
            i++;
        }
        if (i == needle_len) {
            return true;
        }
    }
    return false;
}

/**
 * @brief 通过 esp_tls 写完整个缓冲区。
 *
 * 调用方法：发送 HTTP Upgrade 请求和 WebSocket frame 时使用。esp_tls_conn_write()
 * 可能只写入部分数据，因此必须循环，直到全部写完或底层 TLS 返回错误。
 */
static esp_err_t manual_ws_tls_write_all(esp_tls_t *tls, const uint8_t *data, size_t data_len)
{
    if (tls == NULL || (data == NULL && data_len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t written_total = 0;
    while (written_total < data_len) {
        ssize_t written = esp_tls_conn_write(tls,
                                             data + written_total,
                                             data_len - written_total);
        if (written <= 0) {
            ESP_LOGE(TAG,
                     "manual_ws TLS 写入失败: written=%d, total=%u/%u",
                     (int)written,
                     (unsigned int)written_total,
                     (unsigned int)data_len);
            return ESP_FAIL;
        }
        written_total += (size_t)written;
    }

    return ESP_OK;
}

/**
 * @brief 从 TLS 连接精确读取指定字节数。
 *
 * 调用方法：解析 WebSocket frame 时先读 2 字节基本头，再按需读取扩展长度、
 * mask key 和 payload。底层超时、TLS 错或对端关闭都会转换成 ESP-IDF 错误码。
 */
static esp_err_t manual_ws_tls_read_exact(esp_tls_t *tls, uint8_t *out, size_t out_len)
{
    if (tls == NULL || (out == NULL && out_len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    static uint32_t consecutive_no_data_count;
    size_t read_total = 0;
    while (read_total < out_len) {
        ssize_t read_len = esp_tls_conn_read(tls, out + read_total, out_len - read_total);
        if (read_len > 0) {
            consecutive_no_data_count = 0;
            read_total += (size_t)read_len;
            continue;
        }
        if (read_len == 0) {
            consecutive_no_data_count = 0;
            ESP_LOGW(TAG, "manual_ws 服务器主动关闭 TLS 连接");
            return ESP_ERR_INVALID_STATE;
        }

        if (read_total == 0) {
            consecutive_no_data_count++;
            if ((int)read_len == MANUAL_WS_TLS_READ_WANT_READ) {
                ESP_LOGD(TAG, "manual_ws TLS 暂时无数据: read=%d", (int)read_len);
            } else if ((consecutive_no_data_count % MANUAL_WS_TLS_READ_TIMEOUT_WARN_EVERY) == 0) {
                ESP_LOGW(TAG,
                         "manual_ws TLS 连续读取超时或暂时无数据: count=%" PRIu32 ", read=%d",
                         consecutive_no_data_count,
                         (int)read_len);
            } else {
                ESP_LOGD(TAG, "manual_ws TLS 读取超时或暂时无数据: read=%d", (int)read_len);
            }
            return ESP_ERR_TIMEOUT;
        }

        consecutive_no_data_count = 0;
        ESP_LOGE(TAG,
                 "manual_ws TLS 读取中断: read=%d, total=%u/%u",
                 (int)read_len,
                 (unsigned int)read_total,
                 (unsigned int)out_len);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief 设置当前 TLS 连接底层 socket 的读取超时。
 *
 * 调用方法：manual_ws_connect() 完成 TLS 后和 manual_ws_recv_frame() 每次读帧前调用。
 * esp_tls_conn_read() 走底层 socket 超时，因此这里通过 esp_tls_get_conn_sockfd()
 * 取出 fd，再设置 SO_RCVTIMEO，让 recv_frame(timeout_ms) 的参数真正生效。
 */
static esp_err_t manual_ws_set_read_timeout(manual_ws_client_t *client, int timeout_ms)
{
    if (client == NULL || client->tls == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int sockfd = -1;
    esp_err_t ret = esp_tls_get_conn_sockfd(client->tls, &sockfd);
    if (ret != ESP_OK || sockfd < 0) {
        ESP_LOGE(TAG, "manual_ws TLS 错误: 获取 socket fd 失败: %s", esp_err_to_name(ret));
        return ret != ESP_OK ? ret : ESP_FAIL;
    }

    int effective_timeout_ms = timeout_ms > 0 ? timeout_ms : client->timeout_ms;
    struct timeval tv = {
        .tv_sec = effective_timeout_ms / 1000,
        .tv_usec = (effective_timeout_ms % 1000) * 1000,
    };
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        ESP_LOGE(TAG, "manual_ws TLS 错误: 设置 socket 读取超时失败");
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief 生成 WebSocket 客户端随机 Sec-WebSocket-Key。
 *
 * 调用方法：manual_ws_connect() 组装 HTTP Upgrade 请求前调用。标准要求客户端生成
 * 16 字节随机 nonce，再做 Base64，得到 24 字符字符串。这个 key 不属于业务凭据，
 * 但仍不需要在日志里打印明文。
 */
static esp_err_t manual_ws_make_sec_key(char out[MANUAL_WS_SEC_KEY_BASE64_BUF_LEN])
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t nonce[MANUAL_WS_SEC_KEY_RAW_BYTES] = {0};
    for (size_t i = 0; i < sizeof(nonce); i += sizeof(uint32_t)) {
        uint32_t random_value = esp_random();
        nonce[i] = (uint8_t)(random_value & 0xFFU);
        nonce[i + 1] = (uint8_t)((random_value >> 8) & 0xFFU);
        nonce[i + 2] = (uint8_t)((random_value >> 16) & 0xFFU);
        nonce[i + 3] = (uint8_t)((random_value >> 24) & 0xFFU);
    }

    size_t encoded_len = 0;
    int ret = mbedtls_base64_encode((unsigned char *)out,
                                    MANUAL_WS_SEC_KEY_BASE64_BUF_LEN,
                                    &encoded_len,
                                    nonce,
                                    sizeof(nonce));
    if (ret != 0 || encoded_len != MANUAL_WS_SEC_KEY_BASE64_LEN) {
        out[0] = '\0';
        ESP_LOGE(TAG, "manual_ws Sec-WebSocket-Key 生成失败: ret=%d", ret);
        return ESP_ERR_NO_MEM;
    }

    out[encoded_len] = '\0';
    return ESP_OK;
}

/**
 * @brief 按 RFC6455 计算期望的 Sec-WebSocket-Accept。
 *
 * 调用方法：握手响应头收完整后调用。服务端应返回
 * Base64(SHA1(client_key + MANUAL_WS_GUID))，用于确认对端确实接受了本次 Upgrade。
 */
static esp_err_t manual_ws_make_expected_accept(const char *sec_key,
                                                char out[MANUAL_WS_ACCEPT_BASE64_LEN + 1])
{
    if (sec_key == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char accept_src[MANUAL_WS_SEC_KEY_BASE64_BUF_LEN + sizeof(MANUAL_WS_GUID)] = {0};
    int written = snprintf(accept_src, sizeof(accept_src), "%s%s", sec_key, MANUAL_WS_GUID);
    if (written < 0 || (size_t)written >= sizeof(accept_src)) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t sha1[MANUAL_WS_ACCEPT_SHA1_BYTES] = {0};
    int sha_ret = mbedtls_sha1((const unsigned char *)accept_src,
                               strlen(accept_src),
                               sha1);
    if (sha_ret != 0) {
        ESP_LOGE(TAG, "manual_ws Sec-WebSocket-Accept SHA1 计算失败: ret=%d", sha_ret);
        return ESP_FAIL;
    }

    size_t encoded_len = 0;
    int b64_ret = mbedtls_base64_encode((unsigned char *)out,
                                        MANUAL_WS_ACCEPT_BASE64_LEN + 1,
                                        &encoded_len,
                                        sha1,
                                        sizeof(sha1));
    if (b64_ret != 0 || encoded_len != MANUAL_WS_ACCEPT_BASE64_LEN) {
        out[0] = '\0';
        ESP_LOGE(TAG, "manual_ws Sec-WebSocket-Accept Base64 计算失败: ret=%d", b64_ret);
        return ESP_FAIL;
    }

    out[encoded_len] = '\0';
    return ESP_OK;
}

/**
 * @brief 构造 HTTP Upgrade 请求。
 *
 * 调用方法：manual_ws_connect() 已生成 Sec-WebSocket-Key 后调用。manual_ws 只写标准
 * WebSocket 握手 Header，业务层传入的 extra_headers 原样追加，不在这里写死域名、
 * 路径、X-Api-* 或任何 ASR 字段。
 */
static esp_err_t manual_ws_build_handshake_request(const manual_ws_config_t *config,
                                                   const char *sec_key,
                                                   char **out_request,
                                                   size_t *out_request_len)
{
    if (config == NULL || sec_key == NULL || out_request == NULL || out_request_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *extra_headers = config->extra_headers != NULL ? config->extra_headers : "";
    const char *path = (config->path != NULL && config->path[0] != '\0') ? config->path : "/";
    int port = config->port > 0 ? config->port : 443;
    bool needs_port_in_host = port != 443;
    const char *trailing_crlf = "";
    size_t extra_len = strlen(extra_headers);
    if (extra_len > 0 &&
        !(extra_len >= 2 &&
          extra_headers[extra_len - 2] == '\r' &&
          extra_headers[extra_len - 1] == '\n')) {
        trailing_crlf = "\r\n";
    }

    char host_header[160] = {0};
    int host_header_len = needs_port_in_host ?
        snprintf(host_header, sizeof(host_header), "%s:%d", config->host, port) :
        snprintf(host_header, sizeof(host_header), "%s", config->host);
    if (host_header_len < 0 || (size_t)host_header_len >= sizeof(host_header)) {
        ESP_LOGE(TAG, "manual_ws 握手错误: Host Header 缓冲区不足");
        return ESP_ERR_NO_MEM;
    }

    int request_len = snprintf(NULL,
                               0,
                               "GET %s HTTP/1.1\r\n"
                               "Host: %s\r\n"
                               "Connection: Upgrade\r\n"
                               "Upgrade: websocket\r\n"
                               "Sec-WebSocket-Key: %s\r\n"
                               "Sec-WebSocket-Version: 13\r\n"
                               "%s%s"
                               "\r\n",
                               path,
                               host_header,
                               sec_key,
                               extra_headers,
                               trailing_crlf);
    if (request_len < 0) {
        return ESP_FAIL;
    }

    char *request = (char *)malloc((size_t)request_len + 1U);
    if (request == NULL) {
        ESP_LOGE(TAG, "manual_ws 握手请求 buffer 分配失败: len=%d", request_len);
        return ESP_ERR_NO_MEM;
    }

    int written = snprintf(request,
                           (size_t)request_len + 1U,
                           "GET %s HTTP/1.1\r\n"
                           "Host: %s\r\n"
                           "Connection: Upgrade\r\n"
                           "Upgrade: websocket\r\n"
                           "Sec-WebSocket-Key: %s\r\n"
                           "Sec-WebSocket-Version: 13\r\n"
                           "%s%s"
                           "\r\n",
                           path,
                           host_header,
                           sec_key,
                           extra_headers,
                           trailing_crlf);
    if (written < 0 || written != request_len) {
        free(request);
        return ESP_FAIL;
    }

    *out_request = request;
    *out_request_len = (size_t)request_len;
    return ESP_OK;
}

/**
 * @brief 校验 HTTP Upgrade 响应头。
 *
 * 调用方法：握手读取循环检测到 header complete 后调用。第一版要求响应中包含
 * "HTTP/1.1 101" 和 Sec-WebSocket-Accept，并尽量校验 Accept 值是否匹配本次 key。
 */
static esp_err_t manual_ws_validate_handshake_response(const char *response,
                                                       const char *expected_accept)
{
    if (response == NULL || expected_accept == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strstr(response, "HTTP/1.1 101") == NULL) {
        ESP_LOGE(TAG, "manual_ws 握手错误: 响应头不包含 HTTP/1.1 101");
        return ESP_FAIL;
    }
    if (MANUAL_WS_ENABLE_DEBUG_LOG) {
        manual_ws_log_debug("manual_ws HTTP 101 confirmed");
    }

    if (!manual_ws_contains_case_insensitive(response, "Sec-WebSocket-Accept:")) {
        ESP_LOGE(TAG, "manual_ws 握手错误: 响应头不包含 Sec-WebSocket-Accept");
        return ESP_FAIL;
    }

    if (strstr(response, expected_accept) == NULL) {
        ESP_LOGE(TAG, "manual_ws 握手错误: Sec-WebSocket-Accept 与本次 key 不匹配");
        return ESP_FAIL;
    }

    if (MANUAL_WS_ENABLE_DEBUG_LOG) {
        manual_ws_log_debug("manual_ws Sec-WebSocket-Accept confirmed");
    }
    return ESP_OK;
}

/**
 * @brief 建立 wss:// TLS WebSocket 连接。
 *
 * 握手流程：
 * 1. 使用 esp_tls_init()/esp_tls_conn_new_sync() 建立 TLS，并通过 ESP x509 bundle
 *    校验服务端证书；
 * 2. 生成随机 Sec-WebSocket-Key；
 * 3. 组装 GET path HTTP/1.1 Upgrade 请求，追加业务 extra_headers；
 * 4. 循环读取响应头并累计到固定 buffer；
 * 5. 检测到 "\r\n\r\n" 后校验 HTTP/1.1 101 和 Sec-WebSocket-Accept。
 */
esp_err_t manual_ws_connect(manual_ws_client_t *client, const manual_ws_config_t *config)
{
    if (client == NULL || config == NULL ||
        config->host == NULL || config->host[0] == '\0' ||
        config->path == NULL || config->path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (client->magic == MANUAL_WS_CLIENT_MAGIC) {
        manual_ws_close(client);
    }
    memset(client, 0, sizeof(*client));
    client->magic = MANUAL_WS_CLIENT_MAGIC;
    strlcpy(client->host, config->host, sizeof(client->host));
    strlcpy(client->path, config->path, sizeof(client->path));
    client->port = config->port > 0 ? config->port : 443;
    client->timeout_ms = config->timeout_ms > 0 ?
        config->timeout_ms :
        MANUAL_WS_DEFAULT_TIMEOUT_MS;

    client->tls = esp_tls_init();
    if (client->tls == NULL) {
        ESP_LOGE(TAG, "manual_ws TLS 初始化失败: no memory");
        return ESP_ERR_NO_MEM;
    }

    esp_tls_cfg_t tls_cfg = {
        .crt_bundle_attach = esp_crt_bundle_attach,
        .common_name = client->host,
        .timeout_ms = client->timeout_ms,
    };

    int tls_ret = esp_tls_conn_new_sync(client->host,
                                        strlen(client->host),
                                        client->port,
                                        &tls_cfg,
                                        client->tls);
    if (tls_ret != 1) {
        int tls_code = 0;
        int tls_flags = 0;
        esp_tls_error_handle_t tls_error = NULL;
        esp_tls_get_error_handle(client->tls, &tls_error);
        esp_err_t last_error = esp_tls_get_and_clear_last_error(tls_error, &tls_code, &tls_flags);
        ESP_LOGE(TAG,
                 "manual_ws TLS 错误: ret=%d, last_error=%s, tls_code=0x%x, tls_flags=0x%x",
                 tls_ret,
                 esp_err_to_name(last_error),
                 tls_code,
                 tls_flags);
        manual_ws_close(client);
        return ESP_FAIL;
    }
    if (MANUAL_WS_ENABLE_DEBUG_LOG) {
        manual_ws_log_debug("manual_ws TLS connected: host=%s port=%d", client->host, client->port);
    }
    esp_err_t timeout_ret = manual_ws_set_read_timeout(client, client->timeout_ms);
    if (timeout_ret != ESP_OK) {
        manual_ws_close(client);
        return timeout_ret;
    }

    char sec_key[MANUAL_WS_SEC_KEY_BASE64_BUF_LEN] = {0};
    esp_err_t ret = manual_ws_make_sec_key(sec_key);
    if (ret != ESP_OK) {
        manual_ws_close(client);
        return ret;
    }

    char expected_accept[MANUAL_WS_ACCEPT_BASE64_LEN + 1] = {0};
    ret = manual_ws_make_expected_accept(sec_key, expected_accept);
    if (ret != ESP_OK) {
        manual_ws_close(client);
        return ret;
    }

    char *request = NULL;
    size_t request_len = 0;
    ret = manual_ws_build_handshake_request(config, sec_key, &request, &request_len);
    if (ret != ESP_OK) {
        manual_ws_close(client);
        return ret;
    }

    ret = manual_ws_tls_write_all(client->tls, (const uint8_t *)request, request_len);
    free(request);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "manual_ws 握手错误: HTTP Upgrade 请求发送失败");
        manual_ws_close(client);
        return ret;
    }

    char *response = (char *)malloc(MANUAL_WS_HANDSHAKE_BUF_SIZE + 1U);
    if (response == NULL) {
        ESP_LOGE(TAG, "manual_ws 握手响应 buffer 分配失败");
        manual_ws_close(client);
        return ESP_ERR_NO_MEM;
    }

    size_t response_len = 0;
    while (response_len < MANUAL_WS_HANDSHAKE_BUF_SIZE) {
        size_t free_bytes = MANUAL_WS_HANDSHAKE_BUF_SIZE - response_len;
        size_t read_bytes = free_bytes > MANUAL_WS_IO_BUF_SIZE ? MANUAL_WS_IO_BUF_SIZE : free_bytes;
        ssize_t read_len = esp_tls_conn_read(client->tls,
                                             response + response_len,
                                             read_bytes);
        if (read_len > 0) {
            response_len += (size_t)read_len;
            response[response_len] = '\0';
            if (manual_ws_http_header_complete(response, response_len)) {
                if (MANUAL_WS_ENABLE_DEBUG_LOG) {
                    manual_ws_log_debug("manual_ws handshake header complete: bytes=%u",
                                        (unsigned int)response_len);
                }
                break;
            }
            continue;
        }
        if (read_len == 0) {
            ESP_LOGE(TAG, "manual_ws 握手错误: 服务器在响应头完成前主动关闭连接");
            free(response);
            manual_ws_close(client);
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGE(TAG, "manual_ws 握手错误: TLS 读取响应头失败 read=%d", (int)read_len);
        free(response);
        manual_ws_close(client);
        return ESP_FAIL;
    }

    if (!manual_ws_http_header_complete(response, response_len)) {
        ESP_LOGE(TAG,
                 "manual_ws 握手错误: 响应头超过缓存上限 %u 字节仍未完成",
                 (unsigned int)MANUAL_WS_HANDSHAKE_BUF_SIZE);
        free(response);
        manual_ws_close(client);
        return ESP_ERR_NO_MEM;
    }

    ret = manual_ws_validate_handshake_response(response, expected_accept);
    free(response);
    if (ret != ESP_OK) {
        manual_ws_close(client);
        return ret;
    }

    client->connected = true;
    return ESP_OK;
}

/**
 * @brief 发送一帧客户端 WebSocket frame。
 *
 * mask 处理：
 * 客户端发给服务端的 frame 必须设置 mask bit，并携带 4 字节 mask key。本函数用
 * esp_random() 生成 mask key，然后把 payload 每个字节与 mask_key[i % 4] 异或后发送。
 * 上层传入的 payload 不会被原地修改。
 */
static esp_err_t manual_ws_send_frame(manual_ws_client_t *client,
                                      uint8_t opcode,
                                      const uint8_t *payload,
                                      size_t payload_len)
{
    if (client == NULL || client->tls == NULL || !client->connected) {
        return ESP_ERR_INVALID_STATE;
    }
    if (payload == NULL && payload_len > 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (payload_len > 0xFFFFU) {
        ESP_LOGE(TAG,
                 "manual_ws payload 太大: 发送端第一版只支持 <=65535 字节, len=%u",
                 (unsigned int)payload_len);
        return ESP_ERR_NOT_SUPPORTED;
    }

    size_t header_len = MANUAL_WS_BASE_HEADER_BYTES;
    if (payload_len > 125U) {
        header_len += MANUAL_WS_EXT16_BYTES;
    }
    size_t mask_offset = header_len;
    size_t frame_len = header_len + MANUAL_WS_MASK_KEY_BYTES + payload_len;
    uint8_t *frame = (uint8_t *)malloc(frame_len);
    if (frame == NULL) {
        ESP_LOGE(TAG, "manual_ws 发送帧 buffer 分配失败: len=%u", (unsigned int)frame_len);
        return ESP_ERR_NO_MEM;
    }

    frame[0] = (uint8_t)(0x80U | (opcode & 0x0FU));
    if (payload_len <= 125U) {
        frame[1] = (uint8_t)(0x80U | payload_len);
    } else {
        frame[1] = 0x80U | 126U;
        frame[2] = (uint8_t)((payload_len >> 8) & 0xFFU);
        frame[3] = (uint8_t)(payload_len & 0xFFU);
    }

    uint8_t *mask_key = &frame[mask_offset];
    uint32_t random_value = esp_random();
    mask_key[0] = (uint8_t)(random_value & 0xFFU);
    mask_key[1] = (uint8_t)((random_value >> 8) & 0xFFU);
    mask_key[2] = (uint8_t)((random_value >> 16) & 0xFFU);
    mask_key[3] = (uint8_t)((random_value >> 24) & 0xFFU);

    uint8_t *masked_payload = &frame[mask_offset + MANUAL_WS_MASK_KEY_BYTES];
    for (size_t i = 0; i < payload_len; i++) {
        masked_payload[i] = payload[i] ^ mask_key[i % MANUAL_WS_MASK_KEY_BYTES];
    }

    if (MANUAL_WS_ENABLE_DEBUG_LOG) {
        manual_ws_log_debug("manual_ws send frame opcode=0x%02X(%s), payload_len=%u, masked=1",
                            (unsigned int)(opcode & 0x0FU),
                            manual_ws_opcode_name(opcode & 0x0FU),
                            (unsigned int)payload_len);
        if ((opcode & 0x0FU) == MANUAL_WS_OPCODE_BINARY) {
            manual_ws_log_debug("manual_ws send binary frame");
        }
    }

    esp_err_t ret = manual_ws_tls_write_all(client->tls, frame, frame_len);
    free(frame);
    return ret;
}

/**
 * @brief 发送 binary frame。
 *
 * 调用方法：业务层已经组装好自己的二进制协议 payload 后调用；manual_ws 只负责
 * WebSocket binary 封装和 mask。
 */
esp_err_t manual_ws_send_binary(manual_ws_client_t *client, const uint8_t *data, size_t len)
{
    return manual_ws_send_frame(client, MANUAL_WS_OPCODE_BINARY, data, len);
}

/**
 * @brief 发送 text frame。
 *
 * 调用方法：需要发送 UTF-8 文本 WebSocket 消息时调用；字符串结尾 NUL 不会发送。
 */
esp_err_t manual_ws_send_text(manual_ws_client_t *client, const char *text)
{
    if (text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return manual_ws_send_frame(client,
                                MANUAL_WS_OPCODE_TEXT,
                                (const uint8_t *)text,
                                strlen(text));
}

/**
 * @brief 发送 close frame。
 *
 * 调用方法：上层主动关闭 WebSocket 时调用。第一版发送空 payload close frame，不等待
 * close ack，随后 manual_ws_close() 会销毁 TLS 连接。
 */
esp_err_t manual_ws_send_close(manual_ws_client_t *client)
{
    return manual_ws_send_frame(client, MANUAL_WS_OPCODE_CLOSE, NULL, 0);
}

/**
 * @brief 解析 WebSocket payload length。
 *
 * payload_len 规则：
 * - 小于 126：第二个字节低 7 位就是长度；
 * - 等于 126：后续 2 字节是 16 bit big-endian 长度；
 * - 等于 127：后续 8 字节是 64 bit big-endian 长度，第一版不支持超大帧。
 */
static esp_err_t manual_ws_read_payload_length(manual_ws_client_t *client,
                                               uint8_t len7,
                                               uint64_t *out_payload_len)
{
    if (client == NULL || out_payload_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (len7 < 126U) {
        *out_payload_len = len7;
        return ESP_OK;
    }

    if (len7 == 126U) {
        uint8_t ext[MANUAL_WS_EXT16_BYTES] = {0};
        esp_err_t ret = manual_ws_tls_read_exact(client->tls, ext, sizeof(ext));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "manual_ws 帧格式错: 读取 16bit payload_len 失败: %s", esp_err_to_name(ret));
            return ret;
        }
        *out_payload_len = ((uint64_t)ext[0] << 8) | (uint64_t)ext[1];
        return ESP_OK;
    }

    uint8_t ext[MANUAL_WS_EXT64_BYTES] = {0};
    esp_err_t ret = manual_ws_tls_read_exact(client->tls, ext, sizeof(ext));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "manual_ws 帧格式错: 读取 64bit payload_len 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    uint64_t payload_len = 0;
    for (size_t i = 0; i < sizeof(ext); i++) {
        payload_len = (payload_len << 8) | (uint64_t)ext[i];
    }
    ESP_LOGE(TAG,
             "manual_ws 暂不支持 64bit 超大帧: payload_len=%" PRIu64
             "，第一版只支持 <126 和 16bit 扩展长度",
             payload_len);
    return ESP_ERR_NOT_SUPPORTED;
}

/**
 * @brief 接收并解析一帧服务端 WebSocket frame。
 *
 * close/ping/pong 处理：
 * - close：记录服务器主动 close，并把 connected 置 false；
 * - ping：自动发送 masked pong，frame 仍返回给上层便于日志观察；
 * - pong：正常返回，上层可选择忽略；
 * - binary/text：payload 拷贝到 payload_buf，由上层解析业务协议。
 */
esp_err_t manual_ws_recv_frame(manual_ws_client_t *client,
                               manual_ws_frame_t *frame,
                               uint8_t *payload_buf,
                               size_t payload_buf_size,
                               int timeout_ms)
{
    if (client == NULL || frame == NULL ||
        client->tls == NULL || !client->connected ||
        payload_buf == NULL || payload_buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(frame, 0, sizeof(*frame));
    esp_err_t timeout_ret = manual_ws_set_read_timeout(client, timeout_ms);
    if (timeout_ret != ESP_OK) {
        return timeout_ret;
    }

    uint8_t header[MANUAL_WS_BASE_HEADER_BYTES] = {0};
    esp_err_t ret = manual_ws_tls_read_exact(client->tls, header, sizeof(header));
    if (ret != ESP_OK) {
        return ret;
    }

    frame->fin = (header[0] & 0x80U) != 0;
    frame->opcode = header[0] & 0x0FU;
    frame->masked = (header[1] & 0x80U) != 0;

    uint64_t payload_len64 = 0;
    ret = manual_ws_read_payload_length(client, header[1] & 0x7FU, &payload_len64);
    if (ret != ESP_OK) {
        return ret;
    }

    if (payload_len64 > MANUAL_WS_MAX_PAYLOAD_SIZE || payload_len64 > payload_buf_size) {
        ESP_LOGE(TAG,
                 "manual_ws payload 太大: len=%" PRIu64 ", max=%u, buf=%u",
                 payload_len64,
                 (unsigned int)MANUAL_WS_MAX_PAYLOAD_SIZE,
                 (unsigned int)payload_buf_size);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t mask_key[MANUAL_WS_MASK_KEY_BYTES] = {0};
    if (frame->masked) {
        ret = manual_ws_tls_read_exact(client->tls, mask_key, sizeof(mask_key));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "manual_ws 帧格式错: 读取服务端 mask key 失败: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    frame->payload_len = (size_t)payload_len64;
    frame->payload = frame->payload_len > 0 ? payload_buf : NULL;
    if (frame->payload_len > 0) {
        ret = manual_ws_tls_read_exact(client->tls, payload_buf, frame->payload_len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "manual_ws TLS 错误: 读取 payload 失败: %s", esp_err_to_name(ret));
            return ret;
        }
        if (frame->masked) {
            for (size_t i = 0; i < frame->payload_len; i++) {
                payload_buf[i] ^= mask_key[i % MANUAL_WS_MASK_KEY_BYTES];
            }
        }
    }

    if (MANUAL_WS_ENABLE_DEBUG_LOG) {
        manual_ws_log_debug("manual_ws recv frame opcode=0x%02X(%s), payload_len=%u, masked=%d, fin=%d",
                            (unsigned int)frame->opcode,
                            manual_ws_opcode_name(frame->opcode),
                            (unsigned int)frame->payload_len,
                            frame->masked,
                            frame->fin);
        if (frame->opcode == MANUAL_WS_OPCODE_BINARY) {
            manual_ws_log_debug("manual_ws recv binary frame");
        }
    }

    if (frame->opcode == MANUAL_WS_OPCODE_PING) {
        if (MANUAL_WS_ENABLE_DEBUG_LOG) {
            manual_ws_log_debug("manual_ws recv ping frame, auto reply pong");
        }
        return manual_ws_send_frame(client,
                                    MANUAL_WS_OPCODE_PONG,
                                    frame->payload,
                                    frame->payload_len);
    }

    if (frame->opcode == MANUAL_WS_OPCODE_PONG) {
        if (MANUAL_WS_ENABLE_DEBUG_LOG) {
            manual_ws_log_debug("manual_ws recv pong frame");
        }
        return ESP_OK;
    }

    if (frame->opcode == MANUAL_WS_OPCODE_CLOSE) {
        ESP_LOGW(TAG, "manual_ws 服务器主动 close frame");
        client->connected = false;
        return ESP_OK;
    }

    return ESP_OK;
}

/**
 * @brief 关闭 WebSocket/TLS 连接并释放资源。
 *
 * 调用方法：业务完成、异常或超时后调用。若连接仍处于 connected，会先尽力发送 close
 * frame；无论 close frame 是否成功，最终都会销毁 esp_tls 句柄并清空 client 状态。
 */
void manual_ws_close(manual_ws_client_t *client)
{
    if (client == NULL) {
        return;
    }
    if (client->magic != MANUAL_WS_CLIENT_MAGIC) {
        memset(client, 0, sizeof(*client));
        return;
    }

    if (client->tls != NULL) {
        if (client->connected) {
            (void)manual_ws_send_close(client);
        }
        esp_tls_conn_destroy(client->tls);
    }
    memset(client, 0, sizeof(*client));
}
