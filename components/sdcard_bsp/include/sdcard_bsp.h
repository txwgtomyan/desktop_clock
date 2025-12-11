#ifndef __SDCARD_BSP_H_
#define __SDCARD_BSP_H_

#include "esp_err.h"

/**
 * @brief 挂载 SD 卡并注册 FAT 文件系统。
 *
 * @return ESP_OK 成功挂载；其他错误码表示失败。
 */
esp_err_t sdcard_bsp_init(void);

#endif
