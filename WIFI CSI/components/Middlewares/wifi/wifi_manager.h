#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

/**
 * @brief 初始化Wi-Fi管理功能
 *
 * 此函数负责初始化NVS、TCP/IP适配器、事件循环和Wi-Fi。
 * @return 成功返回 ESP_OK，失败返回对应错误码。
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief 连接到已保存列表里信号最强的 Wi-Fi AP
 *
 * 本函数会先扫描周围 AP，再从 wifi_credentials.h 的 WIFI_KNOWN_LIST
 * 中选择 RSSI 最大的一项连接。新增 WiFi 名和密码时，只需要在
 * WIFI_KNOWN_LIST 里新增 {"WiFi名", "WiFi密码"}。
 * @return 成功返回 ESP_OK，失败返回对应错误码。
 */
esp_err_t wifi_connect_to_ap(void);

/**
 * @brief 读取当前已连接的 WiFi 名称
 *
 * @param ssid 用来保存 WiFi 名称的缓冲区。
 * @param ssid_len ssid 缓冲区长度，建议至少 33 字节。
 * @return true 表示读取成功，false 表示当前未连接或参数无效。
 */
bool wifi_get_connected_ssid(char *ssid, size_t ssid_len);

/**
 * @brief 获取Wi-Fi连接状态
 *
 * 此函数返回当前连接状态。
 * @return 已连接返回 true，未连接返回 false。
 */
bool wifi_is_connected(void);

#endif // WIFI_MANAGER_H
