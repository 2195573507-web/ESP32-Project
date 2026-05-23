#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief 初始化Wi-Fi管理功能
 *
 * 此函数负责初始化NVS、TCP/IP适配器、事件循环和Wi-Fi。
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief 启动Wi-Fi扫描
 *
 * 此函数启动Wi-Fi AP扫描，并通过串口发送SSID列表。
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t wifi_scan_start(void);

/**
 * @brief 连接到默认配置的Wi-Fi AP
 *
 * 本函数使用 wifi_manager 组件内部定义的 SSID 和密码进行连接。
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t wifi_connect_to_ap(void);

/**
 * @brief 获取Wi-Fi连接状态
 *
 * 此函数返回当前连接状态。
 * @return true if connected, false otherwise
 */
bool wifi_is_connected(void);

#endif // WIFI_MANAGER_H