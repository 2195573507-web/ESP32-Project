#ifndef APP_BME690_H
#define APP_BME690_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* BME690_READ_INTERVAL_MS：main 示例默认读取周期，单位毫秒。 */
#define BME690_READ_INTERVAL_MS       1000U

/* BME690_HEATER_TEMP_C：强制模式下气体传感器加热目标温度，单位摄氏度。 */
#define BME690_HEATER_TEMP_C          300U

/* BME690_HEATER_DUR_MS：强制模式下气体传感器加热持续时间，单位毫秒。 */
#define BME690_HEATER_DUR_MS          100U

typedef struct {
    float temperature_c;
    float pressure_pa;
    float humidity_percent;
    float gas_resistance_ohm;
    uint8_t status;
} bme690_data_t;

esp_err_t bme690_init(void);
esp_err_t bme690_read_forced(bme690_data_t *out_data);
void bme690_deinit(void);
bool bme690_is_ready(void);

#ifdef __cplusplus
}
#endif

#endif
