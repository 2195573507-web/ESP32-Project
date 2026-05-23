#include <stdio.h>              // 标准输入输出库，用于printf等函数
#include "freertos/FreeRTOS.h"    // FreeRTOS操作系统核心头文件，提供任务调度等功能
#include "freertos/task.h"        // FreeRTOS任务管理头文件，用于创建和管理任务
#include "freertos/event_groups.h" // FreeRTOS事件组头文件，用于同步和通信
#include "esp_system.h"           // ESP32系统相关函数，如重启、获取芯片信息等
#include "esp_wifi.h"             // ESP32 WiFi功能头文件，提供WiFi初始化和配置
#include "esp_event.h"            // ESP32事件系统头文件，用于事件循环和处理器
#include "esp_log.h"              // ESP32日志系统头文件，用于打印调试信息
#include "nvs_flash.h"             // 非易失性存储(NVS)头文件，用于存储WiFi配置等数据
#include "esp_netif.h"            // ESP32网络接口头文件，提供TCP/IP栈接口

// WiFi名称和密码定义
#define DEFAULT_SSID        "REPLACE_WITH_WIFI_SSID"  // 要连接的WiFi名称
#define DEFAULT_PWD         "REPLACE_WITH_WIFI_PASSWORD"  // WiFi密码

// 事件标志位定义
#define WIFI_CONNECTED_BIT BIT0  // WiFi连接成功标志位
#define WIFI_FAIL_BIT      BIT1  // WiFi连接失败标志位

// 静态变量
static EventGroupHandle_t wifi_event_group;  // WiFi事件组句柄，用于同步WiFi事件
static const char *TAG = "wifi_station";     // 日志标签，用于标识日志来源
static int s_retry_num = 0;                  // 重试次数计数器

// WiFi事件处理器函数
// 处理WiFi连接过程中的各种事件，如启动、断开、获取IP等
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        // WiFi STA模式启动，开始连接WiFi
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        // WiFi断开连接，尝试重连
        if (s_retry_num < 5) {  // 最多重试5次
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "重试连接WiFi...");
        } else {
            // 连接失败，设置失败标志
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"连接WiFi失败");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        // 获取到IP地址，连接成功
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "获取到IP地址:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;  // 重置重试次数
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);  // 设置连接成功标志
    }
}

// WiFi初始化函数
// 初始化WiFi模块，配置连接参数，注册事件处理器
void wifi_init_sta(void)
{
    // 初始化TCP/IP网络接口，为网络通信做准备
    ESP_ERROR_CHECK(esp_netif_init());

    // 创建默认事件循环，用于处理系统事件
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 创建默认WiFi STA网络接口，用于WiFi连接
    esp_netif_create_default_wifi_sta();

    // 初始化WiFi配置，使用默认配置
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 注册WiFi事件处理器，监听WiFi相关事件
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    // 配置WiFi连接参数，包括SSID、密码等
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = DEFAULT_SSID,      // 设置WiFi名称
            .password = DEFAULT_PWD,   // 设置WiFi密码
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,  // 认证模式
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));  // 设置为STA模式（客户端模式）
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));  // 应用WiFi配置
    ESP_ERROR_CHECK(esp_wifi_start());  // 启动WiFi

    ESP_LOGI(TAG, "WiFi初始化完成，正在连接到 %s ...", DEFAULT_SSID);
}

// 主函数
// 程序入口点，初始化系统并启动WiFi连接
void app_main(void)
{
    // 初始化NVS（非易失性存储），用于存储WiFi配置等数据
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // 如果NVS分区被截断，需要擦除并重新初始化
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 创建WiFi事件组，用于同步WiFi连接状态
    wifi_event_group = xEventGroupCreate();

    // 初始化WiFi
    wifi_init_sta();

    // 等待WiFi连接结果，阻塞直到连接成功或失败
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        // WiFi连接成功
        ESP_LOGI(TAG, "WiFi连接成功");
    } else if (bits & WIFI_FAIL_BIT) {
        // WiFi连接失败
        ESP_LOGI(TAG, "WiFi连接失败");
    } else {
        // 意外情况
        ESP_LOGE(TAG, "意外错误");
    }
}
