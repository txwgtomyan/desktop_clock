#ifndef COMPONENTS_RTC_BSP_INCLUDE_RTC_BSP_H_
#define COMPONENTS_RTC_BSP_INCLUDE_RTC_BSP_H_

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize RTC peripheral (PCF85063A) over I2C
 *
 * Returns ESP_OK on success or an esp_err_t on failure.
 */
esp_err_t rtc_bsp_init(void);

#ifdef __cplusplus
}
#endif

#endif // COMPONENTS_RTC_BSP_INCLUDE_RTC_BSP_H_
