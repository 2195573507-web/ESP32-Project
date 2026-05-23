#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <nvs_flash.h>
#include "wifi_credentials.h"
#include "wifi_manager.h"

static const char *TAG = "wifi_manager";

/* FreeRTOS 事件组：用于标记 Wi-Fi 是否已经连接。 */
static EventGroupHandle_t s_wifi_event_group;

/* 事件组位：连接成功和连接失败分别占用一个 bit。 */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;
static const int MAX_RETRY = 5;

/**
 * @brief 在已知 WiFi 列表中按 SSID 查找账号。
 *
 * 调用方法：scan_strongest_known_wifi() 扫描到 AP 后调用。
 * @param ssid 扫描到的 WiFi 名称，大小写敏感。
 * @return 找到时返回 WIFI_KNOWN_LIST 中对应项指针，未找到返回 NULL。
 */
static const known_wifi_t *find_known_wifi_by_ssid(const char *ssid)
{
    for (size_t i = 0; i < WIFI_KNOWN_COUNT; i++) {
        if (strcmp(ssid, WIFI_KNOWN_LIST[i].ssid) == 0) {
            return &WIFI_KNOWN_LIST[i];
        }
    }

    return NULL;
}

/**
 * @brief 扫描周围 AP，并选择 RSSI 最强的已知 WiFi。
 *
 * 调用方法：wifi_connect_to_ap() 在真正连接前调用。
 * @param selected_wifi 输出参数，返回选中的已知 WiFi 账号指针。
 * @param selected_rssi 输出参数，返回选中 AP 的 RSSI。
 * @return 成功找到已知 WiFi 返回 ESP_OK，否则返回错误码。
 */
static esp_err_t scan_strongest_known_wifi(const known_wifi_t **selected_wifi, int8_t *selected_rssi)
{
    *selected_wifi = NULL;
    *selected_rssi = INT8_MIN;

    ESP_LOGI(TAG, "Scanning known Wi-Fi...");

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 60,
        .scan_time.active.max = 80,
    };

    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(ret));
        return ret;
    }

    uint16_t ap_count = 0;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));
    ESP_LOGI(TAG, "Found %d access points", ap_count);

    if (ap_count == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    wifi_ap_record_t *ap_list = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (ap_list == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for AP list");
        return ESP_ERR_NO_MEM;
    }

    ret = esp_wifi_scan_get_ap_records(&ap_count, ap_list);
    if (ret != ESP_OK) {
        free(ap_list);
        ESP_LOGE(TAG, "Get AP records failed: %s", esp_err_to_name(ret));
        return ret;
    }

    for (uint16_t i = 0; i < ap_count; i++) {
        const known_wifi_t *known_wifi = find_known_wifi_by_ssid((const char *)ap_list[i].ssid);
        if (known_wifi == NULL) {
            continue;
        }

        if (*selected_wifi == NULL || ap_list[i].rssi > *selected_rssi) {
            *selected_wifi = known_wifi;
            *selected_rssi = ap_list[i].rssi;
        }
    }

    free(ap_list);

    if (*selected_wifi == NULL) {
        ESP_LOGW(TAG, "No known Wi-Fi found");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Selected Wi-Fi: %s, RSSI: %d", (*selected_wifi)->ssid, *selected_rssi);
    return ESP_OK;
}

/**
 * @brief 事件处理函数
 *
 * 处理Wi-Fi和IP事件。
 * 调用方法：wifi_manager_init() 通过 esp_event_handler_register() 注册，
 * ESP-IDF 在 WiFi/IP 事件发生时自动调用。
 * @param arg 注册事件处理函数时传入的用户参数，本模块未使用。
 * @param event_base 事件类型，当前处理 WIFI_EVENT 和 IP_EVENT。
 * @param event_id 事件 ID，用于区分 STA_START、STA_DISCONNECTED、STA_GOT_IP。
 * @param event_data 事件数据，STA_GOT_IP 时转换为 ip_event_got_ip_t。
 */
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Wi-Fi STA started");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retry to connect to the AP, attempt %d", s_retry_num);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGI(TAG, "Failed to connect to the AP");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/**
 * @brief 初始化 WiFi 管理器。
 *
 * 参数：无。
 * 调用方法：app_main() 启动时先调用，成功后才能调用 wifi_connect_to_ap()。
 * @return 成功返回 ESP_OK，失败由 ESP_ERROR_CHECK 触发或返回错误码。
 */
esp_err_t wifi_manager_init(void)
{
    // 初始化NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 初始化TCP/IP适配器
    ESP_ERROR_CHECK(esp_netif_init());

    // 创建默认事件循环
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 创建STA模式的默认Wi-Fi网络接口
    esp_netif_create_default_wifi_sta();

    // 初始化Wi-Fi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 创建事件组
    s_wifi_event_group = xEventGroupCreate();

    // 注册事件处理函数
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    // 设置Wi-Fi模式为STA
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // 启动Wi-Fi
    ESP_ERROR_CHECK(esp_wifi_start());

    // 关闭 WiFi 省电，CSI 采集需要持续接收无线帧，省电模式可能导致长时间没有数据。
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "Wi-Fi manager initialized");
    return ESP_OK;
}

/**
 * @brief 连接到当前扫描结果中信号最强的已知 WiFi。
 *
 * 参数：无。
 * 调用方法：wifi_manager_init() 成功后调用。
 * @return 连接成功返回 ESP_OK，失败返回 ESP_FAIL 或扫描相关错误码。
 */
esp_err_t wifi_connect_to_ap(void)
{
    const known_wifi_t *selected_wifi = NULL;
    int8_t selected_rssi = 0;

    esp_err_t ret = scan_strongest_known_wifi(&selected_wifi, &selected_rssi);
    if (ret != ESP_OK) {
        return ret;
    }

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };

    // 复制扫描后选中的 WiFi 名和密码，确保末尾以 NUL 结束。
    strlcpy((char *)wifi_config.sta.ssid, selected_wifi->ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, selected_wifi->password, sizeof(wifi_config.sta.password));

    // 连接前清除上一次连接留下的状态位，避免误判连接结果。
    s_retry_num = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    // 设置 WiFi 配置。
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    ESP_LOGI(TAG, "Connecting to SSID: %s, RSSI: %d", selected_wifi->ssid, selected_rssi);

    // 连接到扫描结果中信号最强的已知 WiFi。
    esp_wifi_connect();

    // 等待连接结果
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP");
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to AP");
        return ESP_FAIL;
    } else {
        ESP_LOGE(TAG, "Unexpected event");
        return ESP_FAIL;
    }
}

/**
 * @brief 查询当前 WiFi 是否已经连接。
 *
 * 参数：无。
 * 调用方法：需要判断连接状态时调用。
 * @return 已连接返回 true，未连接返回 false。
 */
bool wifi_is_connected(void)
{
    return (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
}

/**
 * @brief 读取当前已连接 WiFi 的 SSID。
 *
 * 参数：
 *   ssid: 输出缓冲区，用于保存 WiFi 名称。
 *   ssid_len: 输出缓冲区长度，建议至少 33 字节。
 * 调用方法：wifi_connect_to_ap() 成功后调用。
 * @return 读取成功返回 true，未连接或读取失败返回 false。
 */
bool wifi_get_connected_ssid(char *ssid, size_t ssid_len)
{
    if (ssid == NULL || ssid_len == 0 || !wifi_is_connected()) {
        return false;
    }

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        return false;
    }

    strlcpy(ssid, (const char *)ap_info.ssid, ssid_len);
    return true;
}
