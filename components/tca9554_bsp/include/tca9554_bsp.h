#ifndef __TCA9554_BSP_H_
#define __TCA9554_BSP_H_

#include <stdint.h>
#include "esp_err.h"

#define LCD_BL_PIN (1ULL << 1)  // 背光控制引脚，P1 -> 位1

/**
 * @brief 初始化 TCA9554 拓展 IO（仅需调用一次）
 */
esp_err_t tca9554_bsp_init(void);

/**
 * @brief 设置拓展 IO 电平
 * @param pin_num 引脚号（0~7）
 * @param level   电平值 0/1
 */
esp_err_t exio_pin_set_value(uint8_t pin_num, uint8_t level);

/**
 * @brief 读取拓展 IO 电平
 * @param pin_num 引脚号（0~7）
 * @param level   输出参数，返回电平
 */
esp_err_t exio_pin_read_value(uint8_t pin_num, uint32_t *level);

#endif
