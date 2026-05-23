#ifndef LCD_H
#define LCD_H

#include "esp_err.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_ops.h"

// 初始化函数
void lcd_init(void);
void lcd_fill_color(uint16_t color);

#endif // LCD_H