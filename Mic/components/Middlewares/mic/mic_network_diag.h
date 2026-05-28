#ifndef MIC_NETWORK_DIAG_H
#define MIC_NETWORK_DIAG_H

#include "esp_err.h"

/**
 * @file mic_network_diag.h
 * @brief Mic 云端链路网络诊断模块。
 *
 * 本模块用于在启动豆包 ASR 前快速判断网络卡在哪一层：
 * 1. DNS：域名 openspeech.bytedance.com 是否能解析到 IP。
 * 2. TCP 443：设备是否能连到服务端 443 端口。
 *
 * 为了给 ASR WebSocket/TLS 留出堆内存，默认不再做 TLS 握手诊断；
 * 诊断只验证 DNS 和 TCP 可达性，不发送豆包 ASR WebSocket 握手 Header，也不涉及 API Key。
 */

/*
 * 自动网络诊断开关：
 * 0 = 默认关闭，WiFi 稳定后直接进入 ASR 测试流程；
 * 1 = 启动 ASR 前额外运行 DNS/TCP 443 诊断，方便排查基础网络连通性。
 *
 * 这里使用 #ifndef，便于后续通过编译参数覆盖，而不必改源码。
 */
#ifndef MIC_NETWORK_DIAG_ENABLE
#define MIC_NETWORK_DIAG_ENABLE 0
#endif

/*
 * TLS 诊断开关：
 * 0 = 默认关闭，避免 ASR 前额外分配 mbedTLS 输入/输出缓冲和证书校验资源；
 * 1 = 手动排查证书/TLS 问题时才开启，开启后 mic_network_diag_run() 会在 TCP 通过后继续测 TLS。
 */
#ifndef MIC_NETWORK_DIAG_TLS_ENABLE
#define MIC_NETWORK_DIAG_TLS_ENABLE 0
#endif

/* 豆包 ASR 网络诊断目标。后续如果 ASR 域名变化，只需要同步修改这里。 */
#define MIC_NETWORK_DIAG_HOST           "openspeech.bytedance.com" // 豆包 ASR 域名。
#define MIC_NETWORK_DIAG_PORT           443                         // HTTPS/WSS 标准端口。
#define MIC_NETWORK_DIAG_CONNECT_MS     8000                        // TCP 443 连接超时。
#define MIC_NETWORK_DIAG_TLS_TIMEOUT_MS 15000                       // 可选 TLS 握手超时，默认不执行 TLS 诊断。

/**
 * @brief 运行一次豆包 ASR 网络诊断。
 *
 * 调用方法：WiFi 已连接并稳定后调用一次。默认只按 DNS、TCP 443 顺序测试；
 * 任一步失败都会停止后续测试，并在日志中打印失败原因和 errno/ESP-IDF 错误码。
 *
 * @return 默认 DNS/TCP 全部通过返回 ESP_OK；否则返回对应错误码。
 */
esp_err_t mic_network_diag_run(void);

#endif // MIC_NETWORK_DIAG_H
