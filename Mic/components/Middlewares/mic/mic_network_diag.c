#include "mic_network_diag.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_log.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#if MIC_NETWORK_DIAG_TLS_ENABLE
#include "esp_crt_bundle.h"
#include "esp_tls.h"
#endif

static const char *TAG = "mic_network_diag";

enum {
    MIC_NETWORK_DIAG_IP_TEXT_MAX_LEN = 48,
    MIC_NETWORK_DIAG_SERVICE_TEXT_MAX_LEN = 8,
};

/**
 * @brief 把 sockaddr 中的 IP 地址转成日志字符串。
 *
 * 调用方法：DNS 成功、TCP 尝试连接前调用，用来打印最终测试的目标 IP。
 *
 * @param addr getaddrinfo() 返回的地址结构，不能为空。
 * @param out 输出字符串缓冲区，不能为空。
 * @param out_len 输出缓冲区长度。
 * @return 转换成功返回 true，地址族不支持或缓冲区无效返回 false。
 */
static bool mic_network_diag_addr_to_text(const struct sockaddr *addr, char *out, size_t out_len)
{
    if (addr == NULL || out == NULL || out_len == 0) {
        return false;
    }

    const void *src = NULL;
    if (addr->sa_family == AF_INET) {
        const struct sockaddr_in *ipv4 = (const struct sockaddr_in *)addr;
        src = &ipv4->sin_addr;
    } else if (addr->sa_family == AF_INET6) {
        const struct sockaddr_in6 *ipv6 = (const struct sockaddr_in6 *)addr;
        src = &ipv6->sin6_addr;
    } else {
        return false;
    }

    return inet_ntop(addr->sa_family, src, out, out_len) != NULL;
}

/**
 * @brief 把端口写入 getaddrinfo() 返回的 sockaddr。
 *
 * 调用方法：DNS 查询只关心域名，连接前再把 MIC_NETWORK_DIAG_PORT 写入地址结构。
 *
 * @param addr 需要设置端口的地址结构，不能为空。
 * @param port 目标 TCP 端口。
 */
static void mic_network_diag_set_addr_port(struct sockaddr *addr, uint16_t port)
{
    if (addr == NULL) {
        return;
    }

    if (addr->sa_family == AF_INET) {
        struct sockaddr_in *ipv4 = (struct sockaddr_in *)addr;
        ipv4->sin_port = htons(port);
    } else if (addr->sa_family == AF_INET6) {
        struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)addr;
        ipv6->sin6_port = htons(port);
    }
}

/**
 * @brief 解析豆包 ASR 域名。
 *
 * 调用方法：mic_network_diag_run() 第一步调用。成功时通过 out_addr 返回
 * getaddrinfo() 的结果链表，调用者必须 freeaddrinfo()。
 *
 * @param out_addr 输出 DNS 结果链表，不能为空。
 * @return 成功返回 ESP_OK；失败返回 ESP_ERR_NOT_FOUND。
 */
static esp_err_t mic_network_diag_resolve_dns(struct addrinfo **out_addr)
{
    if (out_addr == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_addr = NULL;

    struct addrinfo hints = {
        .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
        .ai_protocol = IPPROTO_TCP,
    };

    ESP_LOGI(TAG, "DNS test start: %s", MIC_NETWORK_DIAG_HOST);
    int gai_ret = getaddrinfo(MIC_NETWORK_DIAG_HOST, NULL, &hints, out_addr);
    if (gai_ret != 0 || *out_addr == NULL) {
        ESP_LOGE(TAG,
                 "DNS test failed: host=%s, getaddrinfo=%d, errno=%d",
                 MIC_NETWORK_DIAG_HOST,
                 gai_ret,
                 errno);
        return ESP_ERR_NOT_FOUND;
    }

    char ip_text[MIC_NETWORK_DIAG_IP_TEXT_MAX_LEN] = {0};
    if (mic_network_diag_addr_to_text((*out_addr)->ai_addr, ip_text, sizeof(ip_text))) {
        ESP_LOGI(TAG, "DNS test ok: %s -> %s", MIC_NETWORK_DIAG_HOST, ip_text);
    } else {
        ESP_LOGI(TAG, "DNS test ok: %s resolved", MIC_NETWORK_DIAG_HOST);
    }

    return ESP_OK;
}

/**
 * @brief 设置 socket 为非阻塞模式。
 *
 * 调用方法：TCP connect 前调用，这样可以用 select() 做明确的连接超时控制。
 *
 * @param sock socket 文件描述符。
 * @return 成功返回 ESP_OK；失败返回 ESP_FAIL。
 */
static esp_err_t mic_network_diag_set_socket_nonblocking(int sock)
{
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) {
        ESP_LOGE(TAG, "fcntl(F_GETFL) failed: errno=%d", errno);
        return ESP_FAIL;
    }

    if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        ESP_LOGE(TAG, "fcntl(F_SETFL, O_NONBLOCK) failed: errno=%d", errno);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief 等待非阻塞 TCP connect 完成。
 *
 * 调用方法：connect() 返回 EINPROGRESS 后调用。select() 可写不一定代表成功，
 * 所以还要读取 SO_ERROR，只有 SO_ERROR 为 0 才表示 TCP 三次握手成功。
 *
 * @param sock socket 文件描述符。
 * @return 连接成功返回 ESP_OK；超时返回 ESP_ERR_TIMEOUT；失败返回 ESP_FAIL。
 */
static esp_err_t mic_network_diag_wait_tcp_connected(int sock)
{
    fd_set write_set;
    FD_ZERO(&write_set);
    FD_SET(sock, &write_set);

    struct timeval timeout = {
        .tv_sec = MIC_NETWORK_DIAG_CONNECT_MS / 1000,
        .tv_usec = (MIC_NETWORK_DIAG_CONNECT_MS % 1000) * 1000,
    };

    int select_ret = select(sock + 1, NULL, &write_set, NULL, &timeout);
    if (select_ret == 0) {
        ESP_LOGE(TAG, "TCP 443 test timeout after %d ms", MIC_NETWORK_DIAG_CONNECT_MS);
        return ESP_ERR_TIMEOUT;
    }
    if (select_ret < 0) {
        ESP_LOGE(TAG, "TCP 443 select failed: errno=%d", errno);
        return ESP_FAIL;
    }

    int socket_error = 0;
    socklen_t socket_error_len = sizeof(socket_error);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_len) < 0) {
        ESP_LOGE(TAG, "TCP 443 getsockopt(SO_ERROR) failed: errno=%d", errno);
        return ESP_FAIL;
    }
    if (socket_error != 0) {
        ESP_LOGE(TAG, "TCP 443 connect failed: socket_error=%d", socket_error);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief 测试是否能连通豆包 ASR 的 TCP 443 端口。
 *
 * 调用方法：DNS 成功后调用。函数会逐个尝试 DNS 返回的地址，只要任意地址
 * TCP 连接成功就返回 ESP_OK；全部失败才返回最后一次错误。
 *
 * @param addr_list getaddrinfo() 返回的地址链表，不能为空。
 * @return TCP 443 可达返回 ESP_OK；全部地址失败返回错误码。
 */
static esp_err_t mic_network_diag_test_tcp_443(const struct addrinfo *addr_list)
{
    if (addr_list == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char service[MIC_NETWORK_DIAG_SERVICE_TEXT_MAX_LEN] = {0};
    snprintf(service, sizeof(service), "%d", MIC_NETWORK_DIAG_PORT);
    ESP_LOGI(TAG,
             "TCP 443 test start: %s:%s, timeout=%d ms",
             MIC_NETWORK_DIAG_HOST,
             service,
             MIC_NETWORK_DIAG_CONNECT_MS);

    esp_err_t last_error = ESP_FAIL;
    for (const struct addrinfo *addr = addr_list; addr != NULL; addr = addr->ai_next) {
        if (addr->ai_addr == NULL ||
            (addr->ai_family != AF_INET && addr->ai_family != AF_INET6)) {
            continue;
        }

        if (addr->ai_addrlen > sizeof(struct sockaddr_storage)) {
            ESP_LOGW(TAG, "TCP 443 skip oversized sockaddr: len=%u", (unsigned int)addr->ai_addrlen);
            continue;
        }

        struct sockaddr_storage target_addr = {0};
        memcpy(&target_addr, addr->ai_addr, addr->ai_addrlen);
        mic_network_diag_set_addr_port((struct sockaddr *)&target_addr, MIC_NETWORK_DIAG_PORT);

        char ip_text[MIC_NETWORK_DIAG_IP_TEXT_MAX_LEN] = {0};
        if (!mic_network_diag_addr_to_text((const struct sockaddr *)&target_addr,
                                           ip_text,
                                           sizeof(ip_text))) {
            strlcpy(ip_text, "(unknown)", sizeof(ip_text));
        }

        int sock = socket(addr->ai_family, SOCK_STREAM, IPPROTO_TCP);
        if (sock < 0) {
            ESP_LOGE(TAG, "TCP 443 socket create failed: ip=%s, errno=%d", ip_text, errno);
            last_error = ESP_FAIL;
            continue;
        }

        esp_err_t ret = mic_network_diag_set_socket_nonblocking(sock);
        if (ret != ESP_OK) {
            close(sock);
            last_error = ret;
            continue;
        }

        int connect_ret = connect(sock, (const struct sockaddr *)&target_addr, addr->ai_addrlen);
        if (connect_ret == 0) {
            ESP_LOGI(TAG, "TCP 443 test ok: %s:%d", ip_text, MIC_NETWORK_DIAG_PORT);
            close(sock);
            return ESP_OK;
        }
        if (errno == EINPROGRESS) {
            ret = mic_network_diag_wait_tcp_connected(sock);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "TCP 443 test ok: %s:%d", ip_text, MIC_NETWORK_DIAG_PORT);
                close(sock);
                return ESP_OK;
            }
            last_error = ret;
        } else {
            ESP_LOGE(TAG, "TCP 443 connect failed immediately: ip=%s, errno=%d", ip_text, errno);
            last_error = ESP_FAIL;
        }

        close(sock);
    }

    ESP_LOGE(TAG, "TCP 443 test failed for all resolved addresses");
    return last_error;
}

#if MIC_NETWORK_DIAG_TLS_ENABLE
/**
 * @brief 测试是否能完成到豆包 ASR 域名的 TLS 握手。
 *
 * 调用方法：只有 MIC_NETWORK_DIAG_TLS_ENABLE=1 时才编译和调用。默认关闭，
 * 因为 TLS 诊断会额外分配 mbedTLS 缓冲和证书校验资源，容易抢占后续 ASR WebSocket 的堆内存。
 *
 * @return TLS 握手成功返回 ESP_OK；失败返回 ESP_FAIL 或 ESP_ERR_TIMEOUT。
 */
static esp_err_t mic_network_diag_test_tls(void)
{
    ESP_LOGI(TAG,
             "TLS test start: %s:%d, timeout=%d ms",
             MIC_NETWORK_DIAG_HOST,
             MIC_NETWORK_DIAG_PORT,
             MIC_NETWORK_DIAG_TLS_TIMEOUT_MS);

    esp_tls_t *tls = esp_tls_init();
    if (tls == NULL) {
        ESP_LOGE(TAG, "TLS test failed: esp_tls_init no memory");
        return ESP_ERR_NO_MEM;
    }

    esp_tls_cfg_t cfg = {
        .crt_bundle_attach = esp_crt_bundle_attach,
        .common_name = MIC_NETWORK_DIAG_HOST,
        .timeout_ms = MIC_NETWORK_DIAG_TLS_TIMEOUT_MS,
    };

    int tls_ret = esp_tls_conn_new_sync(MIC_NETWORK_DIAG_HOST,
                                        strlen(MIC_NETWORK_DIAG_HOST),
                                        MIC_NETWORK_DIAG_PORT,
                                        &cfg,
                                        tls);
    if (tls_ret != 1) {
        int esp_tls_code = 0;
        int esp_tls_flags = 0;
        esp_tls_error_handle_t tls_error = NULL;
        esp_tls_get_error_handle(tls, &tls_error);
        esp_err_t last_error =
            esp_tls_get_and_clear_last_error(tls_error, &esp_tls_code, &esp_tls_flags);
        ESP_LOGE(TAG,
                 "TLS test failed: ret=%d, last_error=%s, tls_code=0x%x, tls_flags=0x%x",
                 tls_ret,
                 esp_err_to_name(last_error),
                 esp_tls_code,
                 esp_tls_flags);
        esp_tls_conn_destroy(tls);
        if (tls_ret == 0) {
            return ESP_ERR_TIMEOUT;
        }
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "TLS test ok: %s:%d", MIC_NETWORK_DIAG_HOST, MIC_NETWORK_DIAG_PORT);
    esp_tls_conn_destroy(tls);
    return ESP_OK;
}
#endif

esp_err_t mic_network_diag_run(void)
{
    ESP_LOGI(TAG,
             "Network diag start: target=%s:%d",
             MIC_NETWORK_DIAG_HOST,
             MIC_NETWORK_DIAG_PORT);

    struct addrinfo *addr_list = NULL;
    esp_err_t ret = mic_network_diag_resolve_dns(&addr_list);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = mic_network_diag_test_tcp_443(addr_list);
    freeaddrinfo(addr_list);
    addr_list = NULL;
    if (ret != ESP_OK) {
        return ret;
    }

#if MIC_NETWORK_DIAG_TLS_ENABLE
    // 只有手动打开 TLS 诊断时才执行；默认跳过，避免 ASR WebSocket 前重复做一次 TLS 大内存分配。
    ret = mic_network_diag_test_tls();
    if (ret != ESP_OK) {
        return ret;
    }
    ESP_LOGI(TAG, "Network diag ok: DNS, TCP 443 and TLS all passed");
#else
    ESP_LOGI(TAG, "Network diag ok: DNS and TCP 443 passed, TLS diag skipped");
#endif
    return ESP_OK;
}
