#ifndef COMPONENTS_LCD_BSP_INCLUDE_LCD_BSP_H_
#define COMPONENTS_LCD_BSP_INCLUDE_LCD_BSP_H_

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 LCD 面板与 I/O（SPI/QSPI + 面板驱动）
 *
 * 调用后面板完成硬件复位与上电初始化命令。
 * @return ESP_OK 表示成功，其他错误码表示失败。
 */
esp_err_t lcd_bsp_init(void);

/**
 * @brief 初始化触摸 I2C 设备（仅创建 device 句柄，不阻塞轮询）
 * @return ESP_OK 成功，其他错误码表示失败
 */
esp_err_t lcd_touch_bsp_init(void);

/**
 * @brief 读取一次触摸坐标
 * @param[out] x 触摸 X 坐标（若无触摸可返回上次值或 0）
 * @param[out] y 触摸 Y 坐标
 * @return ESP_OK 成功，ESP_ERR_INVALID_STATE 表示未初始化，其他为 I2C 错误
 */
esp_err_t lcd_touch_read_point(uint16_t *x, uint16_t *y);
esp_err_t lcd_touch_bsp_init(void);
#ifdef __cplusplus
}
#endif

#endif // COMPONENTS_LCD_BSP_INCLUDE_LCD_BSP_H_
