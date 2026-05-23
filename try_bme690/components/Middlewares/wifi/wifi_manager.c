#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <nvs_flash.h>
#include "wifi_manager.h"

static const char *TAG = "wifi_manager";

static const char *wifi_ssid = "REPLACE_WITH_WIFI_SSID";
static const char *wifi_password = "REPLACE_WITH_WIFI_PASSWORD";

//static const char *wifi_ssid = "EXAMPLE_WIFI_SSID";
//static const char *wifi_password = "EXAMPLE_WIFI_PASSWORD";

static bool wifi_connected_reported = false; // 标记是否已报告连接成功

/* FreeRTOS event group to signal when we are connected */
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;
static const int MAX_RETRY = 5;

/**
 * @brief 事件处理函数
 *
 * 处理Wi-Fi和IP事件。
 * @param arg 用户参数
 * @param event_base 事件基础
 * @param event_id 事件ID
 * @param event_data 事件数据
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
            // 发送连接状态到串口
            printf("{\"wifi_status\":\"retrying\",\"attempt\":%d}\n", s_retry_num);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGI(TAG, "Failed to connect to the AP");
            // 发送连接失败状态到串口
            printf("{\"wifi_status\":\"failed\"}\n");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        // 发送连接成功状态和IP信息到串口
        printf("{\"wifi_status\":\"connected\",\"ip\":\"" IPSTR "\",\"gateway\":\"" IPSTR "\",\"netmask\":\"" IPSTR "\"}\n",
               IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.gw), IP2STR(&event->ip_info.netmask));
    }
}

/**
 * @brief Wi-Fi状态监控任务
 *
 * 定期发送Wi-Fi连接状态和数据使用情况到串口。
 * @param pvParameters 参数（未使用）
 */
static void wifi_status_monitor_task(void *pvParameters)
{
    while (1) {
        if (wifi_is_connected()) {
            if (!wifi_connected_reported) {
                wifi_ap_record_t ap_info;
                esp_netif_ip_info_t ip_info;

                // 获取连接的AP信息
                if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                    // 获取IP信息
                    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip_info);

                    // 发送连接成功状态和IP信息到串口（只输出一次）
                    printf("Wi-Fi connected successfully! IP: " IPSTR "\n", IP2STR(&ip_info.ip));
                    wifi_connected_reported = true;
                }
            }
        } else {
            if (wifi_connected_reported) {
                // 如果之前连接过，现在断开，重置标记
                wifi_connected_reported = false;
            }
        }

        // 每10秒检查一次
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

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

    // 创建状态监控任务
    xTaskCreate(wifi_status_monitor_task, "wifi_status_monitor", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Wi-Fi manager initialized");
    return ESP_OK;
}

esp_err_t wifi_scan_start(void)
{
    ESP_LOGI(TAG, "Starting Wi-Fi scan...");

    // 配置扫描参数
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 120,
        .scan_time.active.max = 150,
    };

    // 启动扫描
    ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_config, true));

    // 获取扫描结果
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    ESP_LOGI(TAG, "Found %d access points", ap_count);

    if (ap_count == 0) {
        return ESP_OK;
    }

    // 分配内存存储AP记录
    wifi_ap_record_t *ap_list = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (ap_list == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for AP list");
        return ESP_ERR_NO_MEM;
    }

    // 获取AP记录
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, ap_list));

    // 以JSON格式发送SSID列表到串口
    printf("{\"wifi_ssids\":[");
    for (int i = 0; i < ap_count; i++) {
        printf("\"%s\"", ap_list[i].ssid);
        if (i < ap_count - 1) {
            printf(",");
        }
    }
    printf("]}");

    // 释放内存
    free(ap_list);

    ESP_LOGI(TAG, "Wi-Fi scan completed, SSID list sent via UART");
    return ESP_OK;
}

esp_err_t wifi_connect_to_ap(void)
{
    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };

    // 复制默认 SSID 和密码，确保末尾以 NUL 结束
    strlcpy((char *)wifi_config.sta.ssid, wifi_ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, wifi_password, sizeof(wifi_config.sta.password));

    // 设置Wi-Fi配置
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    ESP_LOGI(TAG, "Connecting to SSID: %s", wifi_ssid);

    // 连接到AP
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

bool wifi_is_connected(void)
{
    return (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
}
