#include "csi_monitor.h"
#include "csi_feature.h"
#include "csi_processor.h"
#include "wifi_manager.h"
#if CSI_SERIAL_OUTPUT_ENABLE
#include "csi_serial_output.h"
#endif

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_event.h"
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
static portMUX_TYPE s_link_filter_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_feature_lock = portMUX_INITIALIZER_UNLOCKED;
static esp_ping_handle_t s_internal_ping = NULL;
static bool s_csi_event_handler_registered = false;

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
 * @brief 读取当前 CSI 链路过滤条件快照。
 *
 * 调用方法：CSI 回调、串口输出和看门任务需要读取链路状态时调用。
 * @param filter 输出：当前链路过滤条件。
 * @return true 表示链路已经就绪，false 表示 WiFi 当前未连接或过滤条件未初始化。
 */
static bool csi_link_filter_get(csi_link_filter_t *filter)
{
    if (filter == NULL) {
        return false;
    }

    portENTER_CRITICAL(&s_link_filter_lock);
    *filter = s_link_filter;
    portEXIT_CRITICAL(&s_link_filter_lock);

    return filter->ready;
}

/**
 * @brief 清空最近一帧 CSI 快照，避免断线后串口继续输出旧数据。
 *
 * 调用方法：WiFi 断开事件处理函数调用。
 */
static void csi_latest_feature_clear(void)
{
    portENTER_CRITICAL(&s_feature_lock);
    memset(&s_latest_feature, 0, sizeof(s_latest_feature));
    s_csi_rx_count = 0;
    portEXIT_CRITICAL(&s_feature_lock);
}

/**
 * @brief 将 CSI 链路标记为不可用。
 *
 * 调用方法：WiFi 断开事件处理函数调用，串口输出会因此暂停。
 */
static void csi_link_filter_mark_down(void)
{
    portENTER_CRITICAL(&s_link_filter_lock);
    s_link_filter.ready = false;
    portEXIT_CRITICAL(&s_link_filter_lock);

    csi_latest_feature_clear();
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
    csi_link_filter_t filter = {0};

    if (info == NULL || !csi_link_filter_get(&filter)) {
        return false;
    }

    if (!csi_mac_equal(info->mac, filter.ap_bssid)) {
        return false;
    }

    // 只保留 AP/路由器发给本机 STA 的单播帧，避免 beacon、广播和其他设备流量混入。
    if (csi_mac_is_zero(info->dmac) || csi_mac_is_broadcast(info->dmac)) {
        return false;
    }

    return csi_mac_equal(info->dmac, filter.sta_mac);
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

    uint16_t raw_len = info->len > CSI_RAW_MAX_LEN ? CSI_RAW_MAX_LEN : info->len;

    if (!csi_processor_process(info->buf,
                               raw_len,
                               info->first_word_invalid,
                               &raw_processed)) {
        return;
    }

    csi_feature_t feature = {
        .frame_count = frame_count,
        .rssi = rx_ctrl->rssi,
        .channel = rx_ctrl->channel,
        .csi_len = info->len,
        .raw_processed = raw_processed,
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
    csi_link_filter_t filter = {0};

    if (feature == NULL || count == NULL) {
        return false;
    }

    if (!csi_link_filter_get(&filter) || !wifi_is_connected()) {
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
    csi_link_filter_t filter = {0};

    if (link_info == NULL || !csi_link_filter_get(&filter)) {
        return false;
    }

    memcpy(link_info->ap_bssid, filter.ap_bssid, sizeof(link_info->ap_bssid));
    memcpy(link_info->sta_mac, filter.sta_mac, sizeof(link_info->sta_mac));
    link_info->gateway_ip = filter.gateway_ip;
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
        csi_link_filter_t filter = {0};

        if (!csi_link_filter_get(&filter)) {
            ESP_LOGW(TAG, "CSI paused: WiFi link is not ready");
            last_seen_count = s_csi_seen_count;
            continue;
        }

        if (s_csi_rx_count == 0) {
            ESP_LOGW(TAG,
                     "No router->ESP32-C5 CSI yet. seen=%lu, filter src=%02x:%02x:%02x:%02x:%02x:%02x dst=%02x:%02x:%02x:%02x:%02x:%02x",
                     (unsigned long)s_csi_seen_count,
                     filter.ap_bssid[0],
                     filter.ap_bssid[1],
                     filter.ap_bssid[2],
                     filter.ap_bssid[3],
                     filter.ap_bssid[4],
                     filter.ap_bssid[5],
                     filter.sta_mac[0],
                     filter.sta_mac[1],
                     filter.sta_mac[2],
                     filter.sta_mac[3],
                     filter.sta_mac[4],
                     filter.sta_mac[5]);
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
    csi_link_filter_t filter = {0};
    wifi_ap_record_t ap_info;
    ESP_RETURN_ON_ERROR(esp_wifi_sta_get_ap_info(&ap_info), TAG, "Get connected AP info failed");
    ESP_RETURN_ON_ERROR(esp_wifi_get_mac(WIFI_IF_STA, filter.sta_mac), TAG, "Get STA MAC failed");

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

    memcpy(filter.ap_bssid, ap_info.bssid, sizeof(filter.ap_bssid));
    filter.gateway_ip = ip_info.gw;
    filter.ready = true;

    portENTER_CRITICAL(&s_link_filter_lock);
    s_link_filter = filter;
    portEXIT_CRITICAL(&s_link_filter_lock);

    ESP_LOGI(TAG,
             "CSI filter ready: router/AP " MACSTR " -> ESP32-C5 " MACSTR ", gateway " IPSTR,
             MAC2STR(filter.ap_bssid),
             MAC2STR(filter.sta_mac),
             IP2STR(&filter.gateway_ip));
    return ESP_OK;
}

/**
 * @brief 停止并释放内部 ping 会话。
 *
 * 调用方法：WiFi 断开或重新配置网关前调用。
 */
static void internal_ping_stop(void)
{
    if (s_internal_ping == NULL) {
        return;
    }

    (void)esp_ping_stop(s_internal_ping);
    (void)esp_ping_delete_session(s_internal_ping);
    s_internal_ping = NULL;
}

/**
 * @brief 启动内部 ping，用网关回包持续触发 AP -> ESP32-C5 的 CSI。
 *
 * 调用方法：wifi_csi_monitor_start() 在 CSI 回调和任务注册完成后调用。
 * @return 成功返回 ESP_OK，失败返回对应错误码。
 */
static esp_err_t internal_ping_start(void)
{
    internal_ping_stop();

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

    s_internal_ping = ping;
    ESP_LOGI(TAG, "Internal ping started, target gateway: " IPSTR, IP2STR(&ip_info.gw));
    return ESP_OK;
}

/**
 * @brief WiFi 状态变化处理函数，用于暂停和恢复 CSI 串口输出。
 *
 * 调用方法：wifi_csi_monitor_start() 注册到 ESP-IDF 事件循环。
 */
static void csi_wifi_event_handler(void *arg, esp_event_base_t event_base,
                                   int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        internal_ping_stop();
        csi_link_filter_mark_down();
        (void)esp_wifi_set_csi(false);
        (void)esp_wifi_set_promiscuous(false);
        ESP_LOGW(TAG, "CSI paused until WiFi reconnects");
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "WiFi reconnected, refreshing CSI link");

        if (csi_link_filter_init() != ESP_OK) {
            csi_link_filter_mark_down();
            return;
        }

        esp_err_t ret = esp_wifi_set_promiscuous(true);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Re-enable promiscuous mode failed: %s", esp_err_to_name(ret));
        }

        ret = esp_wifi_set_csi(true);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Re-enable CSI failed: %s", esp_err_to_name(ret));
        }

        ret = internal_ping_start();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Restart internal ping failed: %s", esp_err_to_name(ret));
        }
    }
}

/**
 * @brief 注册 CSI 需要的 WiFi/IP 事件处理函数。
 */
static esp_err_t csi_wifi_event_handler_register(void)
{
    if (s_csi_event_handler_registered) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT,
                                                   WIFI_EVENT_STA_DISCONNECTED,
                                                   &csi_wifi_event_handler,
                                                   NULL),
                        TAG,
                        "Register CSI WiFi disconnect event failed");

    esp_err_t ret = esp_event_handler_register(IP_EVENT,
                                               IP_EVENT_STA_GOT_IP,
                                               &csi_wifi_event_handler,
                                               NULL);
    if (ret != ESP_OK) {
        esp_event_handler_unregister(WIFI_EVENT,
                                     WIFI_EVENT_STA_DISCONNECTED,
                                     &csi_wifi_event_handler);
        ESP_LOGW(TAG, "Register CSI WiFi reconnect event failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_csi_event_handler_registered = true;
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

    ESP_RETURN_ON_ERROR(csi_wifi_event_handler_register(), TAG, "Register CSI WiFi event handler failed");
    ESP_RETURN_ON_ERROR(internal_ping_start(), TAG, "Start internal ping failed");
    ESP_LOGI(TAG, "ESP32-C5 RX CSI started");
    return ESP_OK;
}
