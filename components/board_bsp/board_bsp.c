#include "board_bsp.h"
#include "board_config.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tca9554_bsp.h"
#include "sdcard_bsp.h"
#include "rtc_bsp.h"
#include "lvgl_bsp.h"

static const char *TAG = "BOARD_BSP";
/* I2C 总线句柄：0 号用于通用外设，1 号留给触摸等设备 */
i2c_master_bus_handle_t i2c_bus_handle[MAX_I2C_BUS_NUM];


/**
 * @brief 初始化两路 I2C 主机总线
 *        bus_handle[0]：SDA/SCL = I2C_MASTER_SDA_IO / I2C_MASTER_SCL_IO
 *        bus_handle[1]：SDA/SCL = Touch_SDA_NUM / Touch_SCL_NUM
 * @note I2C0 用于通用外设，I2C1 用于触摸屏
 */
static void i2c_master_bus_init(i2c_master_bus_handle_t *bus_handle) {
  i2c_master_bus_config_t i2c_bus_config = {
      .i2c_port = I2C_NUM_0,
      .sda_io_num = I2C_MASTER_SDA_IO,
      .scl_io_num = I2C_MASTER_SCL_IO,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };
  ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &bus_handle[0]));
  i2c_bus_config.scl_io_num = Touch_SCL_NUM;
  i2c_bus_config.sda_io_num = Touch_SDA_NUM;
  i2c_bus_config.i2c_port = I2C_NUM_1;
  ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &bus_handle[1]));
}


/**
 * @brief 初始化板级外设：先建两路 I2C，再初始化 TCA9554 并点亮背光
 */
void board_bsp_init(void) {

  /* 初始化I2C总线 */
  i2c_master_bus_init(i2c_bus_handle);
  ESP_LOGI(TAG, "I2C bus initialization successful");

  /* 初始化拓展IO芯片 */
  esp_err_t ret = tca9554_bsp_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "TCA9554 init failed: %s", esp_err_to_name(ret));
    return;
  }

  lvgl_bsp_init();
  
  /* 设置拓展IO的值 */
  vTaskDelay(pdMS_TO_TICKS(10));
  ret = exio_pin_set_value(LCD_BL_PIN, 1);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Set LCD_BL_PIN failed: %s", esp_err_to_name(ret));
  }

  rtc_bsp_init();

  /* 初始化SD卡 */
  sdcard_bsp_init();
}