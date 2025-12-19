
/*
 * main.c
 * 示例程序：ESP32S3 + AXS15231B LCD 驱动与 LVGL (v9) 移植示例
 * 本文件已添加详细中文注释，便于理解各模块职责与调用流程。
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_err.h"
#include "esp_log.h"

#include "lvgl.h"
#include "lv_demos.h"
#include "esp_lcd_axs15231b.h"
#include "user_config.h"
#include "i2c_bsp.h"
#include "lcd_bl_pwm_bsp.h"

/* 日志 TAG 用于输出调试信息 */
static const char *TAG = "example";

/* LVGL 线程同步互斥量，确保 LVGL API 在多任务中安全调用 */
static SemaphoreHandle_t lvgl_mux = NULL;
/* 用于显示刷新完成通知的信号量（与中断/回调配合） */
static SemaphoreHandle_t flush_done_semaphore = NULL;
/* 当需要进行像素旋转时的目标缓冲区（在 SPIRAM 中分配） */
uint8_t *lvgl_dest = NULL;

/* DMA 传输缓冲区（用于 spi dma 传输），要求可 DMA 访问 */
static uint16_t *trans_buf_1;

/* 显示相关宏定义：位深、每像素字节数、LVGL 缓冲区大小 */
#define LCD_BIT_PER_PIXEL 16
#define BYTES_PER_PIXEL (LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565))
#define BUFF_SIZE (EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * BYTES_PER_PIXEL)

/* LVGL 任务与滴答相关定义 */
#define LVGL_TICK_PERIOD_MS    5
#define LVGL_TASK_MAX_DELAY_MS 500
#define LVGL_TASK_MIN_DELAY_MS 10
#define LVGL_TASK_STACK_SIZE   (8 * 1024)
#define LVGL_TASK_PRIORITY     2

/* 前置声明：背光控制任务 */
static void example_backlight_loop_task(void *arg);

/* AXS15231B 初始化命令数组（设备上电/复位后需要发送的命令序列） */
static const axs15231b_lcd_init_cmd_t lcd_init_cmds[] = 
{
    {0x11, (uint8_t []){0x00}, 0, 100},
    {0x29, (uint8_t []){0x00}, 0, 100},
};

/*
 * 当面板 IO 传输完成时的回调（从 ISR 上下文调用）
 * 通过信号量通知等待传输完成的任务或逻辑。
 */
static bool example_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    BaseType_t high_task_awoken = pdFALSE;
    xSemaphoreGiveFromISR(flush_done_semaphore, &high_task_awoken);
    return false; /* 返回 false 表示不需要调度更高优先级任务（仅通知） */
}

/*
 * LVGL 刷新回调函数
 * 参数:
 *   disp   - LVGL 显示对象
 *   area   - 要刷新的矩形区域
 *   color_p- 指向 LVGL 提供的像素数据（RGB565）
 *
 * 说明：此回调会把 LVGL 绘制的缓存分段发送到 LCD 面板。
 * 在使用 SPIRAM 分段时，按照配置将缓存分段拷贝到 DMA 可访问的缓冲区并调用
 * esp_lcd_panel_draw_bitmap 来发送每一段。回调结束后必须调用 lv_disp_flush_ready
 * 告诉 LVGL 绘制已完成。
 */
static void example_lvgl_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * color_p)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)lv_display_get_user_data(disp);
    /* LVGL 可能以 RGB565 的字节序输出，这里做必要的字节交换以匹配驱动 */
    lv_draw_sw_rgb565_swap(color_p, lv_area_get_width(area) * lv_area_get_height(area));

#if (Rotated == USER_DISP_ROT_90)
    /* 若显示旋转为 90 度，需先对像素进行旋转，然后按分块发送 */
    lv_display_rotation_t rotation = lv_display_get_rotation(disp);
    lv_area_t rotated_area;
    if(rotation != LV_DISPLAY_ROTATION_0)
    {
        lv_color_format_t cf = lv_display_get_color_format(disp);
        /* 计算旋转后区域 */
        rotated_area = *area;
        lv_display_rotate_area(disp, &rotated_area);
        /* 计算源/目标行字节跨度（stride） */
        uint32_t src_stride = lv_draw_buf_width_to_stride(lv_area_get_width(area), cf);
        uint32_t dest_stride = lv_draw_buf_width_to_stride(lv_area_get_width(&rotated_area), cf);
        /* 对像素进行软件旋转，结果写入 lvgl_dest */
        int32_t src_w = lv_area_get_width(area);
        int32_t src_h = lv_area_get_height(area);
        lv_draw_sw_rotate(color_p, lvgl_dest, src_w, src_h, src_stride, dest_stride, rotation, cf);
        /* 后续使用旋转后的区域和缓冲区 */
        area = &rotated_area;
    }

    /* 将大型缓冲区按 LVGL 与 DMA 的分段长度进行分段发送 */
    const int flush_coun = (LVGL_SPIRAM_BUFF_LEN / LVGL_DMA_BUFF_LEN);
    const int offgap = (EXAMPLE_LCD_V_RES / flush_coun);
    const int dmalen = (LVGL_DMA_BUFF_LEN / 2);
    int offsetx1 = 0;
    int offsety1 = 0;
    int offsetx2 = EXAMPLE_LCD_H_RES;
    int offsety2 = offgap;

    uint16_t *map = (uint16_t *)lvgl_dest;
    xSemaphoreGive(flush_done_semaphore); /* 触发一次，使循环中的第一次 xSemaphoreTake 生效 */
    for(int i = 0; i < flush_coun; i++)
    {
        xSemaphoreTake(flush_done_semaphore, portMAX_DELAY);
        memcpy(trans_buf_1, map, LVGL_DMA_BUFF_LEN);
        esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2, offsety2, trans_buf_1);
        offsety1 += offgap;
        offsety2 += offgap;
        map += dmalen;
    }
    xSemaphoreTake(flush_done_semaphore, portMAX_DELAY);
    lv_disp_flush_ready(disp);
#else
    /* 无旋转时直接按段发送 color_p 中的数据 */
    const int flush_coun = (LVGL_SPIRAM_BUFF_LEN / LVGL_DMA_BUFF_LEN);
    const int offgap = (EXAMPLE_LCD_V_RES / flush_coun);
    const int dmalen = (LVGL_DMA_BUFF_LEN / 2);
    int offsetx1 = 0;
    int offsety1 = 0;
    int offsetx2 = EXAMPLE_LCD_H_RES;
    int offsety2 = offgap;

    uint16_t *map = (uint16_t *)color_p;
    xSemaphoreGive(flush_done_semaphore);
    for(int i = 0; i < flush_coun; i++)
    {
        xSemaphoreTake(flush_done_semaphore, portMAX_DELAY);
        memcpy(trans_buf_1, map, LVGL_DMA_BUFF_LEN);
        esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2, offsety2, trans_buf_1);
        offsety1 += offgap;
        offsety2 += offgap;
        map += dmalen;
    }
    xSemaphoreTake(flush_done_semaphore, portMAX_DELAY);
    lv_disp_flush_ready(disp);
#endif
}

/*
 * 触摸读取回调（给 LVGL 提供触摸点）
 * 参数:
 *   indev     - LVGL 输入设备对象
 *   indevData - LVGL 使用的点信息结构体，需在函数内填充
 *
 * 说明：示例通过 I2C 向触摸芯片读取 32 字节数据并解析坐标，
 *       将其转换到 LVGL 坐标系并设置按下/释放状态。
 */
static void TouchInputReadCallback(lv_indev_t * indev, lv_indev_data_t *indevData)
{
    uint8_t read_touchpad_cmd[11] = {0xb5, 0xab, 0xa5, 0x5a, 0x0, 0x0, 0x0, 0x0e, 0x0, 0x0, 0x0};
    uint8_t buff[32] = {0};
    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_master_write_read_dev(disp_touch_dev_handle, read_touchpad_cmd, 11, buff, 32));
    uint16_t pointX;
    uint16_t pointY;
    pointX = (((uint16_t)buff[2] & 0x0f) << 8) | (uint16_t)buff[3];
    pointY = (((uint16_t)buff[4] & 0x0f) << 8) | (uint16_t)buff[5];
    /* buff[1] 表示触摸点数量或状态，按需调整解析规则 */
    if (buff[1] > 0 && buff[1] < 5)
    {
        indevData->state = LV_INDEV_STATE_PRESSED;
#if (Rotated == USER_DISP_ROT_90)
        /* 当屏幕旋转 90 度时，做坐标变换以匹配显示方向 */
        if(pointX > EXAMPLE_LCD_H_RES) pointX = EXAMPLE_LCD_H_RES;
        if(pointY > EXAMPLE_LCD_V_RES) pointY = EXAMPLE_LCD_V_RES;
        indevData->point.x = (EXAMPLE_LCD_H_RES - pointX);
        indevData->point.y = (EXAMPLE_LCD_V_RES - pointY);
#else
        if(pointX > EXAMPLE_LCD_V_RES) pointX = EXAMPLE_LCD_V_RES;
        if(pointY > EXAMPLE_LCD_H_RES) pointY = EXAMPLE_LCD_H_RES;
        indevData->point.x = pointY;
        indevData->point.y = (EXAMPLE_LCD_V_RES - pointX);
#endif
    }
    else
    {
        indevData->state = LV_INDEV_STATE_RELEASED;
    }
}

/*
 * LVGL 滴答增加回调（由定时器触发）
 * 每隔 LVGL_TICK_PERIOD_MS 调用一次，通知 LVGL 时间流逝
 */
static void example_increase_lvgl_tick(void *arg)
{
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

/*
 * 获取 LVGL 互斥锁（带超时）
 * timeout_ms 为 -1 表示无限等待
 */
static bool example_lvgl_lock(int timeout_ms)
{
    assert(lvgl_mux && "bsp_display_start must be called first");
    const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(lvgl_mux, timeout_ticks) == pdTRUE;
}

/* 释放 LVGL 互斥锁 */
static void example_lvgl_unlock(void)
{
    assert(lvgl_mux && "bsp_display_start must be called first");
    xSemaphoreGive(lvgl_mux);
}

/*
 * LVGL 后台任务：循环调用 lv_timer_handler 并根据返回值调整延迟
 * 说明：该任务需持有 lvgl_mux 才能调用 LVGL API
 */
static void example_lvgl_port_task(void *arg)
{
    uint32_t task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
    for(;;)
    {
        if (example_lvgl_lock(-1))
        {
            task_delay_ms = lv_timer_handler();
            example_lvgl_unlock();
        }
        if (task_delay_ms > LVGL_TASK_MAX_DELAY_MS)
        {
            task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
        }
        else if (task_delay_ms < LVGL_TASK_MIN_DELAY_MS)
        {
            task_delay_ms = LVGL_TASK_MIN_DELAY_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
    }
}

/*
 * 应用程序入口
 * 初始化背光、触摸 I2C、SPI 总线、LCD 面板、LVGL，创建任务并启动示例界面
 */
void app_main(void)
{
    /* 初始化背光 PWM（占空比模式） */
    lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);

    /* 创建传输同步信号量，用于面板传输完成通知 */
    flush_done_semaphore = xSemaphoreCreateBinary();
    assert(flush_done_semaphore);

    /* 初始化触摸 I2C 总线（board/driver 层） */
    touch_i2c_master_Init();

    ESP_LOGI(TAG, "Initialize SPI bus");
    /* 配置用于与 LCD 通信的 GPIO（复位引脚） */
    gpio_config_t gpio_conf = {};
    gpio_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_conf.mode = GPIO_MODE_OUTPUT;
    gpio_conf.pin_bit_mask = ((uint64_t)0x01 << EXAMPLE_PIN_NUM_LCD_RST);
    gpio_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));

    /* 初始化 SPI 总线（用于 AXS15231B qSPI/Quad 接口） */
    spi_bus_config_t buscfg = {};
    buscfg.sclk_io_num =  EXAMPLE_PIN_NUM_LCD_PCLK;
    buscfg.data0_io_num = EXAMPLE_PIN_NUM_LCD_DATA0;
    buscfg.data1_io_num = EXAMPLE_PIN_NUM_LCD_DATA1;
    buscfg.data2_io_num = EXAMPLE_PIN_NUM_LCD_DATA2;
    buscfg.data3_io_num = EXAMPLE_PIN_NUM_LCD_DATA3;
    buscfg.max_transfer_sz = LVGL_DMA_BUFF_LEN;
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

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
    io_config.on_color_trans_done = example_notify_lvgl_flush_ready; /* 传输完成回调 */
    io_config.lcd_cmd_bits = 32;
    io_config.lcd_param_bits = 8;
    io_config.flags.quad_mode = true;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_HOST, &io_config, &panel_io));

    /* AXS15231B 厂商扩展配置 */
    axs15231b_vendor_config_t vendor_config = {};
    vendor_config.flags.use_qspi_interface = 1;
    vendor_config.init_cmds = lcd_init_cmds;
    vendor_config.init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]);

    /* 面板设备配置 */
    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = -1;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = LCD_BIT_PER_PIXEL;
    panel_config.vendor_config = &vendor_config;

    ESP_LOGI(TAG, "Install panel driver");
    ESP_ERROR_CHECK(esp_lcd_new_panel_axs15231b(panel_io, &panel_config, &panel));

    /* 硬件复位序列（手动拉高拉低复位引脚）并初始化面板 */
    ESP_ERROR_CHECK(gpio_set_level(EXAMPLE_PIN_NUM_LCD_RST, 1));
    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_ERROR_CHECK(gpio_set_level(EXAMPLE_PIN_NUM_LCD_RST, 0));
    vTaskDelay(pdMS_TO_TICKS(250));
    ESP_ERROR_CHECK(gpio_set_level(EXAMPLE_PIN_NUM_LCD_RST, 1));
    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));

    /* LVGL 初始化与显示缓冲区设置 */
    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();
    lv_display_t * disp = lv_display_create(EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
    lv_display_set_flush_cb(disp, example_lvgl_flush_cb);

    uint8_t *buffer_1 = NULL;
    uint8_t *buffer_2 = NULL;
    buffer_1 = (uint8_t *)heap_caps_malloc(BUFF_SIZE, MALLOC_CAP_SPIRAM);
    assert(buffer_1);
    buffer_2 = (uint8_t *)heap_caps_malloc(BUFF_SIZE, MALLOC_CAP_SPIRAM);
    assert(buffer_2);
    trans_buf_1 = (uint16_t *)heap_caps_malloc(LVGL_DMA_BUFF_LEN, MALLOC_CAP_DMA);
    assert(trans_buf_1);
    lv_display_set_buffers(disp, buffer_1, buffer_2, BUFF_SIZE, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_user_data(disp, panel);
#if (Rotated == USER_DISP_ROT_90)
    /* 如果启用旋转，分配旋转缓存并设置 LVGL 的显示旋转 */
    lvgl_dest = (uint8_t *)heap_caps_malloc(BUFF_SIZE, MALLOC_CAP_SPIRAM);
    lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90);
#endif

    /* LVGL 输入设备（触摸）注册 */
    lv_indev_t *touch_indev = NULL;
    touch_indev = lv_indev_create();
    lv_indev_set_type(touch_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch_indev, TouchInputReadCallback);

    /* 创建 LVGL 滴答定时器，驱动 lv_tick_inc */
    esp_timer_create_args_t lvgl_tick_timer_args = {};
    lvgl_tick_timer_args.callback = &example_increase_lvgl_tick;
    lvgl_tick_timer_args.name = "lvgl_tick";
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    /* 创建 LVGL 互斥锁，保护 LVGL API */
    lvgl_mux = xSemaphoreCreateMutex();
    assert(lvgl_mux);

    /* 创建 LVGL 主循环任务与背光控制任务 */
    xTaskCreatePinnedToCore(example_lvgl_port_task, "LVGL", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, NULL, 0);
    xTaskCreatePinnedToCore(example_backlight_loop_task, "example_backlight_loop_task", 4 * 1024, NULL, 2, NULL, 0);

    /* 在启动时展示 LVGL 示例界面（在互斥锁保护下） */
    if (example_lvgl_lock(-1))
    {
        lv_demo_widgets(); /* 小部件示例界面 */
        /* 如需其它 demo 可取消下面注释 */
        // lv_demo_music();
        // lv_demo_stress();
        // lv_demo_benchmark();

        example_lvgl_unlock();
    }
}

/*
 * 背光控制循环任务（示例）
 * 根据编译宏 Backlight_Testing 在不同亮度间切换或固定延时
 */
static void example_backlight_loop_task(void *arg)
{
    for(;;)
    {
#if  (Backlight_Testing == true)
        vTaskDelay(pdMS_TO_TICKS(1500));
        setUpduty(LCD_PWM_MODE_255);
        vTaskDelay(pdMS_TO_TICKS(1500));
        setUpduty(LCD_PWM_MODE_175);
        vTaskDelay(pdMS_TO_TICKS(1500));
        setUpduty(LCD_PWM_MODE_125);
        vTaskDelay(pdMS_TO_TICKS(1500));
        setUpduty(LCD_PWM_MODE_0);
#else
        vTaskDelay(pdMS_TO_TICKS(2000));
#endif
    }
}
