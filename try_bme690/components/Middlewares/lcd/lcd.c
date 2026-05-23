#include "lcd.h"
#include "esp_lcd_st7789.h"   // 这个必须放在 lcd.c 里，不能放 .h
#include "esp_log.h"

/* 引脚定义 对应你的 ESP32-C5 */
#define SPI2_MOSI_PIN  GPIO_NUM_23
#define SPI2_CLK_PIN   GPIO_NUM_24
#define LCD_CS_PIN      GPIO_NUM_25
#define LCD_DC_PIN      GPIO_NUM_26
#define LCD_RST_PIN     GPIO_NUM_15

// 屏幕电源使能 GPIO5
#define LCD_PWR_PIN     GPIO_NUM_5

#define LCD_SPI_HOST          SPI2_HOST
#define LCD_SPI_CLOCK_HZ      (10 * 1000 * 1000)
#define LCD_H_RES             240
#define LCD_V_RES             284
#define LCD_X_GAP             0
#define LCD_Y_GAP             20
#define LCD_PWR_ACTIVE_LEVEL  1

static const char *TAG = "lcd";

//关键：把 panel_handle 变成全局变量！！
static esp_lcd_panel_handle_t panel_handle = NULL;

void lcd_init(void)
{
    // --------------------------
    // 必须先打开 LCD 电源！！
    // --------------------------
    gpio_config_t pwr_conf = {
        .pin_bit_mask = 1ULL << LCD_PWR_PIN,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&pwr_conf);
    gpio_set_level(LCD_PWR_PIN, LCD_PWR_ACTIVE_LEVEL); // 供电/背光使能（视硬件定义）
    vTaskDelay(pdMS_TO_TICKS(10));

    // --------------------------
    // SPI 总线初始化
    // --------------------------
    spi_bus_config_t bus_config = {
        .mosi_io_num = SPI2_MOSI_PIN,
        .sclk_io_num = SPI2_CLK_PIN,
        .miso_io_num = -1,  // 屏幕不需要 MISO
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * 2, // 一次传输整屏数据，16 位色

    };
    
    ESP_LOGI(TAG, "init: host=%d mosi=%d sclk=%d cs=%d dc=%d rst=%d pwr=%d",
             LCD_SPI_HOST, SPI2_MOSI_PIN, SPI2_CLK_PIN, LCD_CS_PIN, LCD_DC_PIN,
             LCD_RST_PIN, LCD_PWR_PIN);

    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO));
   

    // --------------------------
    // LCD IO 接口配置
    // --------------------------
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = ST7789_PANEL_IO_SPI_CONFIG(
        LCD_CS_PIN,
        LCD_DC_PIN,
        NULL,
        NULL
    );
    io_config.pclk_hz = LCD_SPI_CLOCK_HZ;

    ESP_LOGI(TAG, "io: spi_mode=%d pclk_hz=%d", io_config.spi_mode, (int)io_config.pclk_hz);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_SPI_HOST, &io_config, &io_handle));

    // --------------------------
    // LCD 面板配置
    // --------------------------
    //esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_RST_PIN,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
        .flags.reset_active_high = 0,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));

    // --------------------------
    // 启动屏幕
    // --------------------------
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    vTaskDelay(pdMS_TO_TICKS(100)); // 等待屏幕复位
    
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, LCD_X_GAP, LCD_Y_GAP));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, false, true));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, false));

}

void lcd_fill_color(uint16_t color)
{
    static uint16_t buffer[LCD_H_RES * LCD_V_RES];

    for (int i = 0; i < LCD_H_RES * LCD_V_RES; i++) {
        buffer[i] = color;
    }

    if (panel_handle == NULL) {
        ESP_LOGE(TAG, "panel_handle is NULL");
        return;
    }

    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel_handle,
                                              0, 0,
                                              LCD_H_RES, LCD_V_RES,
                                              buffer));
}
