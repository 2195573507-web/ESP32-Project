#include "csi_monitor.h"
#include "csi_feature.h"
#include "csi_gain_compensation.h"
#include "csi_processor.h"
#if CSI_SERIAL_OUTPUT_ENABLE
#include "csi_serial_output.h"
#endif

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "ping/ping_sock.h"

#define TAG "CSI_MONITOR"

/**
 * @brief router/AP 到 ESP32-C5 的 CSI 链路过滤条件。
 *
 * 调用方法：csi_link_filter_init() 在启动 CSI 前初始化，
 * csi_is_router_to_esp_frame() 在 CSI 回调中使用。
 */
typedef struct {
    uint8_t ap_bssid[6];        // 当前连接 AP/路由器的 BSSID，作为 CSI 源 MAC。
    uint8_t sta_mac[6];         // 本机 STA MAC，作为 CSI 目的 MAC。
    esp_ip4_addr_t gateway_ip;  // 当前网络网关 IP，内部 ping 的目标地址。
    bool ready;                 // true 表示过滤条件已初始化完成。
} csi_link_filter_t;

static csi_link_filter_t s_link_filter = {0};
static volatile uint32_t s_csi_seen_count = 0;
static volatile uint32_t s_csi_rx_count = 0;
static csi_feature_t s_latest_feature = {0};
static portMUX_TYPE s_feature_lock = portMUX_INITIALIZER_UNLOCKED;

/**
 * @brief 比较两个 MAC 地址是否完全相同。
 *
 * 调用方法：本文件内部 MAC 过滤函数调用。
 * @param a 第一个 MAC 地址，长度必须为 6 字节。
 * @param b 第二个 MAC 地址，长度必须为 6 字节。
 * @return 相同返回 true，否则返回 false。
 */
static bool csi_mac_equal(const uint8_t a[6], const uint8_t b[6])
{
    return memcmp(a, b, 6) == 0;
}

/**
 * @brief 判断 MAC 地址是否为全 0。
 *
 * 调用方法：csi_is_router_to_esp_frame() 用它过滤无效目的 MAC。
 * @param mac 待检查 MAC 地址，长度必须为 6 字节。
 * @return 全 0 返回 true，否则返回 false。
 */
static bool csi_mac_is_zero(const uint8_t mac[6])
{
    static const uint8_t zero_mac[6] = {0};
    return csi_mac_equal(mac, zero_mac);
}

/**
 * @brief 判断 MAC 地址是否为广播地址。
 *
 * 调用方法：csi_is_router_to_esp_frame() 用它过滤广播帧。
 * @param mac 待检查 MAC 地址，长度必须为 6 字节。
 * @return 广播地址返回 true，否则返回 false。
 */
static bool csi_mac_is_broadcast(const uint8_t mac[6])
{
    static const uint8_t broadcast_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    return csi_mac_equal(mac, broadcast_mac);
}

/**
 * @brief 判断 CSI 帧是否属于 AP/路由器发给本机 ESP32-C5 的单播下行包。
 *
 * 调用方法：wifi_csi_rx_cb() 收到每帧 CSI 后调用。
 * @param info ESP-IDF WiFi CSI 回调参数。
 * @return true 表示保留该帧，false 表示丢弃。
 */
static bool csi_is_router_to_esp_frame(const wifi_csi_info_t *info)
{
    if (info == NULL || !s_link_filter.ready) {
        return false;
    }

    if (!csi_mac_equal(info->mac, s_link_filter.ap_bssid)) {
        return false;
    }

    // 只保留 AP/路由器发给本机 STA 的单播帧，避免 beacon、广播和其他设备流量混入。
    if (csi_mac_is_zero(info->dmac) || csi_mac_is_broadcast(info->dmac)) {
        return false;
    }

    return csi_mac_equal(info->dmac, s_link_filter.sta_mac);
}

/**
 * @brief WiFi CSI 接收回调。
 *
 * 调用方法：wifi_csi_monitor_start() 通过 esp_wifi_set_csi_rx_cb() 注册，
 * ESP-IDF 在收到 CSI 数据时自动调用。
 * @param ctx 注册回调时传入的用户上下文，本模块未使用。
 * @param info ESP-IDF 提供的 CSI 原始数据、RX 控制信息和 MAC 信息。
 */
static void wifi_csi_rx_cb(void *ctx, wifi_csi_info_t *info)
{
    (void)ctx;

    if (info == NULL || info->buf == NULL) {
        return;
    }

    s_csi_seen_count++;

    if (!csi_is_router_to_esp_frame(info)) {
        return;
    }

    static uint32_t frame_count = 0;

    const wifi_pkt_rx_ctrl_t *rx_ctrl = &info->rx_ctrl;
    csi_processor_result_t raw_processed = {0};
    csi_gain_compensation_result_t gain = {0};

    uint16_t raw_len = info->len > CSI_RAW_MAX_LEN ? CSI_RAW_MAX_LEN : info->len;

    // 先读取官方增益状态，只作为可靠性冻结和辅助幅值特征，不改写 CSI 数据。
    if (csi_gain_compensation_get_state(rx_ctrl, &gain) != ESP_OK) {
        return;
    }

    csi_processor_gain_t processor_gain = {
        .agc_gain = gain.agc_gain,
        .fft_gain = gain.fft_gain,
        .compensate_gain = gain.compensate_gain,
    };

    if (!csi_processor_process(info->buf,
                               raw_len,
                               info->first_word_invalid,
                               &processor_gain,
                               &raw_processed)) {
        return;
    }

    csi_feature_t feature = {
        .frame_count = frame_count,
        .rssi = rx_ctrl->rssi,
        .channel = rx_ctrl->channel,
        .csi_len = info->len,
        .raw_processed = raw_processed,
        .gain = gain,
        .raw_len = raw_len,
        .sig_mode = 0,
        .mcs = 0,
        .cwb = 0,
        .first_word_invalid = info->first_word_invalid,
    };

    memcpy(feature.mac, info->mac, sizeof(feature.mac));
    memcpy(feature.dmac, info->dmac, sizeof(feature.dmac));
#if CSI_PRINT_RAW_DATA
    memcpy(feature.raw_buf, info->buf, raw_len);
#endif

    portENTER_CRITICAL(&s_feature_lock);
    s_latest_feature = feature;
    s_csi_rx_count = frame_count + 1;
    portEXIT_CRITICAL(&s_feature_lock);

    frame_count++;
}

#if CSI_SERIAL_OUTPUT_ENABLE
/**
 * @brief 读取最新一帧 CSI 特征快照，供输出模块或其他消费者使用。
 */
static bool csi_get_latest_feature(csi_feature_t *feature, uint32_t *count, void *ctx)
{
    (void)ctx;

    if (feature == NULL || count == NULL) {
        return false;
    }

    portENTER_CRITICAL(&s_feature_lock);
    *feature = s_latest_feature;
    *count = s_csi_rx_count;
    portEXIT_CRITICAL(&s_feature_lock);

    return true;
}

static bool csi_get_serial_link_info(csi_link_info_t *link_info, void *ctx)
{
    (void)ctx;

    if (link_info == NULL || !s_link_filter.ready) {
        return false;
    }

    memcpy(link_info->ap_bssid, s_link_filter.ap_bssid, sizeof(link_info->ap_bssid));
    memcpy(link_info->sta_mac, s_link_filter.sta_mac, sizeof(link_info->sta_mac));
    link_info->gateway_ip = s_link_filter.gateway_ip;
    return true;
}
#endif

/**
 * @brief CSI 看门任务，用日志提示 CSI 回调是否长时间没有数据。
 *
 * 调用方法：wifi_csi_monitor_start() 通过 xTaskCreate() 创建。
 * @param arg FreeRTOS 任务参数，本模块未使用。
 */
static void wifi_csi_watch_task(void *arg)
{
    (void)arg;
    uint32_t last_seen_count = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));

        if (s_csi_rx_count == 0) {
            ESP_LOGW(TAG,
                     "No router->ESP32-C5 CSI yet. seen=%lu, filter src=%02x:%02x:%02x:%02x:%02x:%02x dst=%02x:%02x:%02x:%02x:%02x:%02x",
                     (unsigned long)s_csi_seen_count,
                     s_link_filter.ap_bssid[0],
                     s_link_filter.ap_bssid[1],
                     s_link_filter.ap_bssid[2],
                     s_link_filter.ap_bssid[3],
                     s_link_filter.ap_bssid[4],
                     s_link_filter.ap_bssid[5],
                     s_link_filter.sta_mac[0],
                     s_link_filter.sta_mac[1],
                     s_link_filter.sta_mac[2],
                     s_link_filter.sta_mac[3],
                     s_link_filter.sta_mac[4],
                     s_link_filter.sta_mac[5]);
        } else if (s_csi_seen_count == last_seen_count) {
            ESP_LOGW(TAG, "CSI callback is quiet. Internal ping is running; check WiFi connection and router reachability.");
        }

        last_seen_count = s_csi_seen_count;
    }
}

/**
 * @brief 初始化 CSI 链路过滤条件。
 *
 * 调用方法：wifi_csi_monitor_start() 在打开 CSI 前调用。
 * @return 成功返回 ESP_OK，失败返回对应错误码。
 */
static esp_err_t csi_link_filter_init(void)
{
    wifi_ap_record_t ap_info;
    ESP_RETURN_ON_ERROR(esp_wifi_sta_get_ap_info(&ap_info), TAG, "Get connected AP info failed");
    ESP_RETURN_ON_ERROR(esp_wifi_get_mac(WIFI_IF_STA, s_link_filter.sta_mac), TAG, "Get STA MAC failed");

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) {
        ESP_LOGW(TAG, "Cannot prepare CSI link filter: WIFI_STA_DEF not found");
        return ESP_ERR_NOT_FOUND;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK || ip_info.gw.addr == 0) {
        ESP_LOGW(TAG, "Cannot prepare CSI link filter: gateway IP is invalid");
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(s_link_filter.ap_bssid, ap_info.bssid, sizeof(s_link_filter.ap_bssid));
    s_link_filter.gateway_ip = ip_info.gw;
    s_link_filter.ready = true;

    ESP_LOGI(TAG,
             "CSI filter ready: router/AP " MACSTR " -> ESP32-C5 " MACSTR ", gateway " IPSTR,
             MAC2STR(s_link_filter.ap_bssid),
             MAC2STR(s_link_filter.sta_mac),
             IP2STR(&s_link_filter.gateway_ip));
    return ESP_OK;
}

/**
 * @brief 启动内部 ping，用网关回包持续触发 AP -> ESP32-C5 的 CSI。
 *
 * 调用方法：wifi_csi_monitor_start() 在 CSI 回调和任务注册完成后调用。
 * @return 成功返回 ESP_OK，失败返回对应错误码。
 */
static esp_err_t internal_ping_start(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) {
        ESP_LOGW(TAG, "Cannot start internal ping: WIFI_STA_DEF not found");
        return ESP_ERR_NOT_FOUND;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK || ip_info.gw.addr == 0) {
        ESP_LOGW(TAG, "Cannot start internal ping: gateway IP is invalid");
        return ESP_ERR_INVALID_STATE;
    }

    ip_addr_t target_addr;
    memset(&target_addr, 0, sizeof(target_addr));
    target_addr.type = IPADDR_TYPE_V4;
    target_addr.u_addr.ip4.addr = ip_info.gw.addr;

    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    ping_config.target_addr = target_addr;
    ping_config.count = ESP_PING_COUNT_INFINITE;
    ping_config.interval_ms = CSI_INTERNAL_PING_INTERVAL_MS;
    ping_config.timeout_ms = 1000;
    ping_config.data_size = CSI_INTERNAL_PING_SIZE;

    esp_ping_callbacks_t callbacks = {0};
    esp_ping_handle_t ping = NULL;
    esp_err_t ret = esp_ping_new_session(&ping_config, &callbacks, &ping);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Create internal ping session failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_ping_start(ping);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Start internal ping failed: %s", esp_err_to_name(ret));
        esp_ping_delete_session(ping);
        return ret;
    }

    ESP_LOGI(TAG, "Internal ping started, target gateway: " IPSTR, IP2STR(&ip_info.gw));
    return ESP_OK;
}

/**
 * @brief 启动 CSI 采集、内部 ping、串口输出和看门任务。
 *
 * 参数：无。
 * 调用方法：app_main() 在 WiFi 连接成功后调用。
 * @return 启动成功返回 ESP_OK，失败返回对应错误码。
 */
esp_err_t wifi_csi_monitor_start(void)
{
    // CSI 需要 WiFi 已经启动并连接；main 中应在 wifi_connect_to_ap() 成功后调用。
    ESP_RETURN_ON_ERROR(csi_link_filter_init(), TAG, "Prepare router->ESP32-C5 CSI filter failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_promiscuous(true), TAG, "Enable promiscuous mode failed");

#if CONFIG_IDF_TARGET_ESP32C5 || CONFIG_IDF_TARGET_ESP32C61
    wifi_csi_config_t csi_config = {
        .enable                   = true,
        .acquire_csi_legacy       = false,
        .acquire_csi_force_lltf   = false,
        .acquire_csi_ht20         = true,
        .acquire_csi_ht40         = false,
        .acquire_csi_vht          = false,
        .acquire_csi_su           = true,
        .acquire_csi_mu           = true,
        .acquire_csi_dcm          = true,
        .acquire_csi_beamformed   = true,
        .acquire_csi_he_stbc_mode = 2,
        .val_scale_cfg            = 0,
        .dump_ack_en              = false,
        .reserved                 = false,
    };
#else
    wifi_csi_config_t csi_config = {
        .lltf_en           = true,
        .htltf_en          = true,
        .stbc_htltf2_en    = true,
        .ltf_merge_en      = true,
        .channel_filter_en = true,
        .manu_scale        = false,
        .shift             = false,
    };
#endif

    ESP_RETURN_ON_ERROR(esp_wifi_set_csi_config(&csi_config), TAG, "Set CSI config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_csi_rx_cb(wifi_csi_rx_cb, NULL), TAG, "Set CSI callback failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_csi(true), TAG, "Enable CSI failed");

#if CSI_SERIAL_OUTPUT_ENABLE
    ESP_RETURN_ON_ERROR(csi_serial_output_start(csi_get_latest_feature,
                                                csi_get_serial_link_info,
                                                NULL),
                        TAG,
                        "Start CSI serial output failed");
#endif

    if (xTaskCreate(wifi_csi_watch_task, "wifi_csi_watch", 3072, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(internal_ping_start(), TAG, "Start internal ping failed");
    ESP_LOGI(TAG, "ESP32-C5 RX CSI started");
    return ESP_OK;
}
