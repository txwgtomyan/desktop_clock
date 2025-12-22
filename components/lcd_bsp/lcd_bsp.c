#include "lcd_bsp.h"

#include "board_config.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_axs15231b.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include <stdint.h>
#include <stdbool.h>

static const char *TAG = "LCD_BSP";
esp_lcd_panel_io_handle_t s_panel_io = NULL;
esp_lcd_panel_handle_t s_panel = NULL;
static uint16_t *s_fill_buf = NULL;
static i2c_master_dev_handle_t s_touch_dev = NULL;
static i2c_master_bus_handle_t s_touch_bus = NULL;

#define LCD_DEFAULT_COLOR 0x1E1E  /**< 默认上电底色：RGB565 */
#define TOUCH_PROBE_RETRY 5        /**< 触摸探测重试次数 */
#define TOUCH_PROBE_DELAY_MS 50    /**< 每次探测间隔 */

/**
 * @brief AXS15231B 初始化命令数组（上电/复位后发送）
 */
static const axs15231b_lcd_init_cmd_t lcd_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 100},
    {0x29, (uint8_t[]){0x00}, 0, 100},
};

/**
 * @brief 使用 DMA buffer 分块填充整屏颜色
 * @param color RGB565 颜色值
 * @return ESP_OK 成功，其他值表示失败
 */
static esp_err_t lcd_bsp_fill_color_internal(uint16_t color)
{
    ESP_RETURN_ON_FALSE(s_panel != NULL, ESP_ERR_INVALID_STATE, TAG, "panel not ready");

    /* 分配 DMA 缓冲（一次性） */
    if (!s_fill_buf) {
        s_fill_buf = heap_caps_malloc(LVGL_DMA_BUFF_LEN, MALLOC_CAP_DMA);
        ESP_RETURN_ON_FALSE(s_fill_buf, ESP_ERR_NO_MEM, TAG, "no DMA buffer");
    }

    /* 计算每次可刷的行数，预填充整块颜色 */
    const size_t pixels_per_chunk = LVGL_DMA_BUFF_LEN / sizeof(uint16_t);
    const size_t max_lines = pixels_per_chunk / EXAMPLE_LCD_H_RES;
    ESP_RETURN_ON_FALSE(max_lines > 0, ESP_ERR_INVALID_SIZE, TAG, "DMA buf too small");

    const size_t chunk_pixels = max_lines * EXAMPLE_LCD_H_RES;
    for (size_t i = 0; i < chunk_pixels; ++i) {
        s_fill_buf[i] = color;
    }

    for (int y = 0; y < EXAMPLE_LCD_V_RES; y += (int)max_lines) {
        int lines = (y + (int)max_lines <= EXAMPLE_LCD_V_RES) ? (int)max_lines : (EXAMPLE_LCD_V_RES - y);

        /* 末块不足整块时，补齐对应像素 */
        if (lines < (int)max_lines) {
            size_t partial_pixels = (size_t)lines * EXAMPLE_LCD_H_RES;
            for (size_t i = 0; i < partial_pixels; ++i) {
                s_fill_buf[i] = color;
            }
        }

        /* 推送当前分块到面板 */
        esp_err_t ret = esp_lcd_panel_draw_bitmap(s_panel, 0, y, EXAMPLE_LCD_H_RES, y + lines, s_fill_buf);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "fill color failed at y=%d: %s", y, esp_err_to_name(ret));
            return ret;
        }
    }

    return ESP_OK;
}

/**
 * @brief 初始化 LCD 面板（SPI/QSPI 总线、面板驱动、硬复位和默认底色）
 * @return ESP_OK 成功，其他错误码表示失败
 */
esp_err_t lcd_bsp_init(void)
{
    ESP_LOGI(TAG, "Initialize SPI bus");

    /* 配置并初始化 SPI 总线 */
    spi_bus_config_t buscfg = {};
    buscfg.sclk_io_num = EXAMPLE_PIN_NUM_LCD_PCLK;
    buscfg.data0_io_num = EXAMPLE_PIN_NUM_LCD_DATA0;
    buscfg.data1_io_num = EXAMPLE_PIN_NUM_LCD_DATA1;
    buscfg.data2_io_num = EXAMPLE_PIN_NUM_LCD_DATA2;
    buscfg.data3_io_num = EXAMPLE_PIN_NUM_LCD_DATA3;
    buscfg.max_transfer_sz = LVGL_DMA_BUFF_LEN;
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Install panel IO");

    /* 配置面板 IO（QSPI） */
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = EXAMPLE_PIN_NUM_LCD_CS;
    io_config.dc_gpio_num = -1;           // DC 未用，走 QSPI
    io_config.spi_mode = 3;               // AXS15231B 采用 SPI 模式 3
    io_config.pclk_hz = 40 * 1000 * 1000; // 40MHz QSPI 时钟
    io_config.trans_queue_depth = 10;     // 队列深度满足连续刷屏
    io_config.lcd_cmd_bits = 32;
    io_config.lcd_param_bits = 8;
    io_config.on_color_trans_done = NULL; // 完成回调，不需要
    io_config.flags.quad_mode = true;     // 启用 QSPI 4 线
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_HOST, &io_config, &s_panel_io));

    /* 厂商配置与面板配置 */
    axs15231b_vendor_config_t vendor_config = {};
    vendor_config.flags.use_qspi_interface = 1;
    vendor_config.init_cmds = lcd_init_cmds;
    vendor_config.init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]);

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = -1; /* 外部硬件复位脚由用户控制 */
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16; /* RGB565 */
    panel_config.vendor_config = &vendor_config;

    ESP_LOGI(TAG, "Install panel driver");
    ESP_ERROR_CHECK(esp_lcd_new_panel_axs15231b(s_panel_io, &panel_config, &s_panel));

    /* 硬件复位时序 */
    ESP_ERROR_CHECK(gpio_set_level(EXAMPLE_PIN_NUM_LCD_RST, 1));
    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_ERROR_CHECK(gpio_set_level(EXAMPLE_PIN_NUM_LCD_RST, 0));
    vTaskDelay(pdMS_TO_TICKS(250));
    ESP_ERROR_CHECK(gpio_set_level(EXAMPLE_PIN_NUM_LCD_RST, 1));
    vTaskDelay(pdMS_TO_TICKS(30));

    /* 初始化面板并刷默认底色 */
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

    ESP_ERROR_CHECK(lcd_bsp_fill_color_internal(LCD_DEFAULT_COLOR));

    return ESP_OK;
}

/**
 * @brief 读取一次触摸坐标
 */
esp_err_t lcd_touch_read_point(uint16_t *x, uint16_t *y)
{
    ESP_RETURN_ON_FALSE(s_touch_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "touch not init");
    if (!x || !y) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 发送读命令并接收数据 */
    const uint8_t read_touchpad_cmd[11] = {0xb5, 0xab, 0xa5, 0x5a, 0x0, 0x0, 0x0, 0x0e, 0x0, 0x0, 0x0};
    uint8_t buff[32] = {0};

    esp_err_t ret = i2c_master_transmit_receive(s_touch_dev, read_touchpad_cmd, sizeof(read_touchpad_cmd), buff, sizeof(buff), pdMS_TO_TICKS(200));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "touch read failed: %s", esp_err_to_name(ret));
        return ret;
    }

    *x = (((uint16_t)buff[2] & 0x0f) << 8) | (uint16_t)buff[3];
    *y = (((uint16_t)buff[4] & 0x0f) << 8) | (uint16_t)buff[5];

    ESP_LOGD(TAG, "Touch X: %u, Y: %u", (unsigned)*x, (unsigned)*y);
    return ESP_OK;
}

esp_err_t lcd_touch_read(bool *pressed, uint16_t *x, uint16_t *y)
{
    ESP_RETURN_ON_FALSE(s_touch_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "touch not init");
    if (!pressed || !x || !y) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t read_touchpad_cmd[11] = {0xb5, 0xab, 0xa5, 0x5a, 0x0, 0x0, 0x0, 0x0e, 0x0, 0x0, 0x0};
    uint8_t buff[32] = {0};

    esp_err_t ret = i2c_master_transmit_receive(s_touch_dev, read_touchpad_cmd, sizeof(read_touchpad_cmd), buff, sizeof(buff), pdMS_TO_TICKS(200));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "touch read failed: %s", esp_err_to_name(ret));
        *pressed = false;
        return ret;
    }

    /* 参考 refer：buff[1] 表示触摸点数量/状态 */
    if (buff[1] > 0 && buff[1] < 5) {
        *pressed = true;
    } else {
        *pressed = false;
    }

    *x = (uint16_t)((((uint16_t)buff[2] & 0x0F) << 8) | (uint16_t)buff[3]);
    *y = (uint16_t)((((uint16_t)buff[4] & 0x0F) << 8) | (uint16_t)buff[5]);

    ESP_LOGD(TAG, "Touch pressed=%d, X=%u, Y=%u", (int)*pressed, (unsigned)*x, (unsigned)*y);
    return ESP_OK;
}

/**
 * @brief 初始化触摸 I2C 设备（非阻塞，不创建轮询任务）
 */
esp_err_t lcd_touch_bsp_init(void)
{
    ESP_LOGI(TAG, "Initialize touch I2C device");
    /* 获取 I2C 总线句柄 */
    ESP_RETURN_ON_ERROR(i2c_master_get_bus_handle(I2C_NUM_1, &s_touch_bus), TAG, "get I2C bus failed");

    /* 配置触摸设备 */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .scl_speed_hz = 300000, 
        .device_address = DISP_TOUCH_ADDR,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_touch_bus, &dev_cfg, &s_touch_dev), TAG, "add touch device failed");

    /* 上电稳定等待 */
    vTaskDelay(pdMS_TO_TICKS(TOUCH_PROBE_DELAY_MS));

    /* 多次探测，减少上电时序导致的 NACK */
    for (int i = 0; i < TOUCH_PROBE_RETRY; ++i) {
        esp_err_t ret = i2c_master_probe(s_touch_bus, DISP_TOUCH_ADDR, pdMS_TO_TICKS(100));
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "touch probe ok on try %d", i + 1);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "touch probe retry %d/%d failed: %s", i + 1, TOUCH_PROBE_RETRY, esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(TOUCH_PROBE_DELAY_MS));
    }

    ESP_RETURN_ON_ERROR(ESP_FAIL, TAG, "touch not responding after retries");

    return ESP_OK;
}

