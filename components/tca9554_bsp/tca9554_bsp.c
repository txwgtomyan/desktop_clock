#include "tca9554_bsp.h"
#include "esp_check.h"
#include "esp_io_expander_tca9554.h"
#include "esp_log.h"

static const char *TAG = "TCA9554_BSP";
/* TCA9554 设备句柄（全局，仅本文件使用） */
static esp_io_expander_handle_t io_expander = NULL;

/**
 * @brief 确认 IO 拓展器已初始化
 *
 * @return ESP_OK 已初始化；否则返回对应错误码
 */
static esp_err_t ensure_ready(void) {
  if (io_expander) {
    return ESP_OK;
  }
  ESP_LOGE(TAG, "IO expander not initialized");
  return ESP_ERR_INVALID_STATE;
}

/**
 * @brief 设置拓展 IO 的电平
 *
 * @param pin_num 引脚号（0~7）
 * @param level   电平值 0/1
 */
esp_err_t exio_pin_set_value(uint8_t pin_num, uint8_t level) {
  ESP_RETURN_ON_ERROR(ensure_ready(), TAG, "not initialized");
  return esp_io_expander_set_level(io_expander, pin_num, level);
}

/**
 * @brief 读取拓展 IO 的电平
 *
 * @param pin_num 引脚号（0~7）
 * @param level   输出参数，读取到的电平值
 */
esp_err_t exio_pin_read_value(uint8_t pin_num, uint32_t *level) {
  ESP_RETURN_ON_FALSE(level, false, TAG, "level pointer is NULL");
  ESP_RETURN_ON_ERROR(ensure_ready(), TAG, "not initialized");
  return esp_io_expander_get_level(io_expander, pin_num, level);
}

/**
 * @brief 初始化 TCA9554 拓展 IO
 *
 * 步骤：
 * 1) 通过 I2C_NUM_0 获取已有的主机总线句柄。
 * 2) 创建 TCA9554 设备句柄。
 * 3) 将 LCD_BL_PIN 配置为输出并默认拉低。
 */
esp_err_t tca9554_bsp_init(void) {
  if (io_expander) {
    return ESP_OK; // 已初始化，无需重复
  }

  i2c_master_bus_handle_t tca9554_i2c_bus_ = NULL;
  ESP_RETURN_ON_ERROR(i2c_master_get_bus_handle(I2C_NUM_0, &tca9554_i2c_bus_),
                      TAG, "get I2C bus failed");

  ESP_RETURN_ON_ERROR(
      esp_io_expander_new_i2c_tca9554(tca9554_i2c_bus_,
                                      ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000,
                                      &io_expander),
      TAG, "create TCA9554 handle failed");

  ESP_RETURN_ON_ERROR(
      esp_io_expander_set_dir(io_expander, LCD_BL_PIN, IO_EXPANDER_OUTPUT), TAG,
      "set dir failed");
  ESP_RETURN_ON_ERROR(esp_io_expander_set_level(io_expander, LCD_BL_PIN, 0),
                      TAG, "set level failed");

  return ESP_OK;
}