#include "lcd_bsp.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "lvgl.h"

#include "esp_lcd_axs15231b.h"
#include "board_config.h"

static const char *TAG = "LCD_BSP";

/* AXS15231B 初始化命令数组（设备上电/复位后需要发送的命令序列） */
static const axs15231b_lcd_init_cmd_t lcd_init_cmds[] = 
{
    {0x11, (uint8_t []){0x00}, 0, 100},
    {0x29, (uint8_t []){0x00}, 0, 100},
};

static bool
example_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io,
                                esp_lcd_panel_io_event_data_t *edata,
                                void *user_ctx) {
  /*
   * 当面板的颜色传输完成时，驱动会调用这里作为回调通知 LVGL：
   * - `user_ctx` 被设置为 LVGL 的 display 指针（在注册回调时传入）
   * - 调用 `lv_display_flush_ready()` 告诉 LVGL
   * 该区域已经被刷新，可进行下一步渲染
   */
  lv_display_t *disp = (lv_display_t *)user_ctx;
  lv_display_flush_ready(disp);
  return false;
}

esp_err_t lcd_bsp_init(void) {
  ESP_LOGI(TAG, "Initialize SPI bus");

  spi_bus_config_t buscfg = {};
  buscfg.sclk_io_num = EXAMPLE_PIN_NUM_LCD_PCLK;
  buscfg.data0_io_num = EXAMPLE_PIN_NUM_LCD_DATA0;
  buscfg.data1_io_num = EXAMPLE_PIN_NUM_LCD_DATA1;
  buscfg.data2_io_num = EXAMPLE_PIN_NUM_LCD_DATA2;
  buscfg.data3_io_num = EXAMPLE_PIN_NUM_LCD_DATA3;
  buscfg.max_transfer_sz = LVGL_DMA_BUFF_LEN;
  ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));

  ESP_LOGI(TAG, "Install panel IO");
  esp_lcd_panel_io_handle_t panel_io = NULL;
  esp_lcd_panel_handle_t panel = NULL;

  /* 配置面板 IO（SPI）参数 */
  esp_lcd_panel_io_spi_config_t io_config = {};
  io_config.cs_gpio_num = EXAMPLE_PIN_NUM_LCD_CS;
  io_config.dc_gpio_num = -1;
  io_config.spi_mode = 3;
  io_config.pclk_hz = 40 * 1000 * 1000;
  io_config.trans_queue_depth = 10;
  io_config.on_color_trans_done =
      example_notify_lvgl_flush_ready; /* 传输完成回调 */
  io_config.lcd_cmd_bits = 32;
  io_config.lcd_param_bits = 8;
  io_config.flags.quad_mode = true;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

  /* AXS15231B 厂商扩展配置 */
    axs15231b_vendor_config_t vendor_config = {};
    vendor_config.flags.use_qspi_interface = 1;
    vendor_config.init_cmds = lcd_init_cmds;
    vendor_config.init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]);

    /* 面板设备配置 */
    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = -1;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    panel_config.vendor_config = &vendor_config;

    ESP_LOGI(TAG, "Install panel driver");
    ESP_ERROR_CHECK(esp_lcd_new_panel_axs15231b(panel_io, &panel_config, &panel));

    ESP_ERROR_CHECK(gpio_set_level(EXAMPLE_PIN_NUM_LCD_RST, 1));
    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_ERROR_CHECK(gpio_set_level(EXAMPLE_PIN_NUM_LCD_RST, 0));
    vTaskDelay(pdMS_TO_TICKS(250));
    ESP_ERROR_CHECK(gpio_set_level(EXAMPLE_PIN_NUM_LCD_RST, 1));
    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));

    
  return ESP_OK;
}