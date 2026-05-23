#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "wifi_manager.h"
#include "OpenAI.h"

#include "lcd.h"

#include "bme69x.h"
#include "iic.h"

#define TAG "AI_APP"

// ===== OpenAI 配置 =====
#define OPENAI_API_KEY       "REPLACE_WITH_OPENAI_API_KEY"
#define OPENAI_BASE_URL      "https://ai-gateway.vei.volces.com/v1/"
#define OPENAI_MODEL_NAME    "doubao-seed-1.6-thinking"
#define OPENAI_SYSTEM_PROMPT "You are a helpful assistant."
#define OPENAI_USER_ID       "esp32_client"

void app_main(void)
{
    ESP_LOGI(TAG, "System start");
    
    struct bme69x_dev dev;
    dev.intf = BME69X_I2C_INTF;// 使用 I2C 接口
    dev.read = iic_read;// 设置读函数指针为 iic_read
    dev.write = iic_write;// 设置写函数指针为 iic_write
    dev.delay_us = iic_delay_us;// 设置延时函数指针为 iic_delay_us
    dev.intf_ptr = &bme_690_i2c_addr;// 将设备结构体的指针赋值给 intf_ptr，供读写函数使用
    bme69x_init();
/*

    // LCD 上电 + 初始化（否则 LCD_PWR_PIN 不会拉高）
    lcd_init();
    lcd_fill_color(0xF800); // 红色 (RGB565)

    // ===== WiFi =====
    wifi_manager_init();
    wifi_scan_start();

    if (wifi_connect_to_ap() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connect failed");
        return;
    }

    ESP_LOGI(TAG, "WiFi connecting...");
    vTaskDelay(pdMS_TO_TICKS(5000));
    ESP_LOGI(TAG, "WiFi connected");

    // ===== OpenAI 初始化 =====
    OpenAI_t *openai = OpenAICreate(OPENAI_API_KEY);
    if (openai == NULL) {
        ESP_LOGE(TAG, "OpenAI create failed");
        return;
    }

    OpenAIChangeBaseURL(openai, OPENAI_BASE_URL);

    OpenAI_ChatCompletion_t *chat = openai->chatCreate(openai);
    if (chat == NULL) {
        ESP_LOGE(TAG, "chatCreate failed");
        OpenAIDelete(openai);
        return;
    }

    chat->setModel(chat, OPENAI_MODEL_NAME);
    chat->setSystem(chat, OPENAI_SYSTEM_PROMPT);
    chat->setUser(chat, OPENAI_USER_ID);

    ESP_LOGI(TAG, "AI ready");

    */

    // 不从串口读取用户输入。保持主循环防止 app_main 退出。
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // ===== 释放资源（基本不会执行）=====
    //openai->chatDelete(chat);
    //OpenAIDelete(openai);
   
}
