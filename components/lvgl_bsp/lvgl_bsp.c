#include <assert.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "board_config.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "lcd_bsp.h"
#include "lvgl.h"
#include "lvgl_bsp.h"
#include <string.h>
#include "esp_log.h"
#include "sys/lock.h"

static const char *TAG = "lvgl_bsp";

extern esp_lcd_panel_handle_t s_panel;
extern esp_lcd_panel_io_handle_t s_panel_io;

/** LVGL 滴答周期（毫秒） */
#define LVGL_BSP_TICK_PERIOD_MS        2
/** LVGL 后台任务最大/最小延时（毫秒） */
#define LVGL_BSP_TASK_MAX_DELAY_MS     500
#define LVGL_BSP_TASK_MIN_DELAY_MS     (1000 / CONFIG_FREERTOS_HZ)
/** LVGL 后台任务资源配置 */
#define LVGL_BSP_TASK_STACK_SIZE       (8 * 1024)
#define LVGL_BSP_TASK_PRIORITY         2

#define LVGL_BSP_MIN(A, B)             ((A) < (B) ? (A) : (B))
#define LVGL_BSP_MAX(A, B)             ((A) > (B) ? (A) : (B))

/**
 * @brief 刷新同步与缓冲
 * - s_flush_done_sem: 面板 IO 颜色传输完成的同步信号量
 * - s_trans_buf:      DMA 可访问的分块传输缓冲
 * - s_lvgl_fb:        LVGL 整屏帧缓冲（优先放入 SPIRAM）
 */
static SemaphoreHandle_t s_flush_done_sem = NULL;
static uint16_t *s_trans_buf = NULL; /* MALLOC_CAP_DMA 缓冲，用于分块传输 */
static uint16_t *s_lvgl_fb = NULL;   /* 整屏帧缓冲（SPIRAM） */

/**
 * @brief 简易 UI：居中数字用于验证刷新
 */
static lv_obj_t *center_label = NULL;
static int center_counter = 0;
static void center_counter_timer_cb(lv_timer_t *timer)
{
	(void)timer;
	center_counter++;
	if (center_label) {
		lv_label_set_text_fmt(center_label, "%d", center_counter);
	}
}

/* 增加一个按钮与其标签：每按一次，按钮上的数字加 1 */
static lv_obj_t *inc_btn = NULL;
static lv_obj_t *inc_btn_label = NULL;
static int inc_count = 0;

static void inc_btn_event_cb(lv_event_t *e)
{
    (void)e;
    inc_count++;
    if (inc_btn_label) {
        lv_label_set_text_fmt(inc_btn_label, "%d", inc_count);
    }
}

/**
 * @brief LVGL 刷新回调（整屏分块传输）
 *
 * 为规避部分面板对局部刷新的兼容性问题，此处忽略 LVGL 的 area，
 * 每次刷新都将整屏帧缓冲按条带分块复制到 DMA 缓冲并发送到面板。
 */
static void example_lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
	esp_lcd_panel_handle_t panel_handle = lv_display_get_user_data(disp);
	/* 参考 refer：忽略局部区域，整屏传输，避免部分刷新不兼容 */
	int offsetx1 = 0;
	int offsety1 = 0;

	int width = EXAMPLE_LCD_H_RES;
	int height = EXAMPLE_LCD_V_RES;
	/* 计算一次可传的最大行数（按 DMA 缓冲长度） */
	const size_t pixels_per_chunk = LVGL_DMA_BUFF_LEN / sizeof(uint16_t);
	int max_lines = (int)(pixels_per_chunk / LVGL_BSP_MAX(width, 1));
	if (max_lines < 1) {
		max_lines = 1;
	}

	const uint16_t *src = (const uint16_t *)px_map; /* FULL 模式下等同 s_lvgl_fb */
	int sent_lines = 0;
	while (sent_lines < height) {
		int lines = LVGL_BSP_MIN(max_lines, height - sent_lines);
		size_t copy_pixels = (size_t)width * lines;

		/* 拷贝当前分块到 DMA 缓冲，并进行 RGB565 字节序交换 */
		memcpy(s_trans_buf, src + (size_t)sent_lines * width, copy_pixels * sizeof(uint16_t));
		lv_draw_sw_rgb565_swap((uint8_t *)s_trans_buf, copy_pixels);

		int y1 = offsety1 + sent_lines;
		/* 结束坐标：可配置为包含或不包含端 */
		int y2 = y1 + lines;            /* 结束坐标采用半开区间 [y1, y2) */
		int x2_end = offsetx1 + width;   /* 同理 [x1, x2) */

		/* 异步提交传输：由 IO 回调在 DMA 完成时发信号 */
		esp_err_t ret = esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, y1, x2_end, y2, s_trans_buf);
		if (ret != ESP_OK) {
			ESP_LOGE(TAG, "draw_bitmap failed: %s", esp_err_to_name(ret));
			break;
		}
		if (s_flush_done_sem) {
			xSemaphoreTake(s_flush_done_sem, portMAX_DELAY);
		}
		sent_lines += lines;
	}

	lv_display_flush_ready(disp);
    (void)width; (void)height; /* 静默未用参数告警 */
}

/**
 * @brief 面板 IO 颜色传输完成回调（ISR 上下文）
 *
 * 仅释放刷新同步信号量，真正的 lv_display_flush_ready() 在刷新回调中完成。
 */
static bool example_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
	/* 由 ISR 通知传输完成，flush 回调中等待该信号后再调用 flush_ready */
	BaseType_t awoken = pdFALSE;
	if (s_flush_done_sem) {
		xSemaphoreGiveFromISR(s_flush_done_sem, &awoken);
	}
	return false;
}

static void example_increase_lvgl_tick(void *arg)
{
	/* 通知 LVGL 经过了多少毫秒 */
	lv_tick_inc(LVGL_BSP_TICK_PERIOD_MS);
}

static _lock_t lvgl_api_lock;

/**
 * @brief LVGL 后台任务
 * 定期调用 lv_timer_handler，并根据返回值动态调整任务延时。
 */
static void example_lvgl_port_task(void *arg)
{
	uint32_t time_till_next_ms = 0;
	while (1) {
		_lock_acquire(&lvgl_api_lock);
		time_till_next_ms = lv_timer_handler();
		_lock_release(&lvgl_api_lock);
		// 防止触发任务看门狗，设置最小延时
		time_till_next_ms = LVGL_BSP_MAX(time_till_next_ms, LVGL_BSP_TASK_MIN_DELAY_MS);
		// 若显示尚未就绪，限制最大延时
		time_till_next_ms = LVGL_BSP_MIN(time_till_next_ms, LVGL_BSP_TASK_MAX_DELAY_MS);

		TickType_t delay_ticks = pdMS_TO_TICKS(time_till_next_ms);
		if (delay_ticks == 0) {
			delay_ticks = 1;
		}
		vTaskDelay(delay_ticks);
	}
}

/**
 * @brief 创建 LVGL 显示并配置整屏帧缓冲（FULL 模式）
 */
static lv_display_t *example_lvgl_create_display_with_buffers(void)
{
	lv_display_t *display = lv_display_create(EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
	/* 分配整屏帧缓冲（SPIRAM优先），供 LVGL 渲染整屏 */
	if (!s_lvgl_fb) {
		s_lvgl_fb = heap_caps_malloc(LVGL_SPIRAM_BUFF_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
		if (!s_lvgl_fb) {
			s_lvgl_fb = heap_caps_malloc(LVGL_SPIRAM_BUFF_LEN, MALLOC_CAP_8BIT);
		}
		assert(s_lvgl_fb);
	}

	lv_display_set_buffers(display, s_lvgl_fb, NULL, LVGL_SPIRAM_BUFF_LEN, LV_DISPLAY_RENDER_MODE_FULL);
	lv_display_set_user_data(display, s_panel);
	lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
	lv_display_set_flush_cb(display, example_lvgl_flush_cb);

	return display;
}

/**
 * @brief 安装并启动 LVGL 滴答定时器
 */
static void example_lvgl_start_tick_timer(void)
{
	static esp_timer_handle_t lvgl_tick_timer = NULL;
	if (lvgl_tick_timer != NULL) {
		return;
	}

	const esp_timer_create_args_t lvgl_tick_timer_args = {
		.callback = &example_increase_lvgl_tick,
		.name = "lvgl_tick",
	};
	ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
	ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, LVGL_BSP_TICK_PERIOD_MS * 1000));
}

/**
 * @brief 注册面板 IO 颜色传输完成回调
 */
static void example_lvgl_register_flush_ready_callback(lv_display_t *display)
{
	const esp_lcd_panel_io_callbacks_t cbs = {
		.on_color_trans_done = example_notify_lvgl_flush_ready,
	};
	ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(s_panel_io, &cbs, display));
}

/**
 * @brief LVGL 触摸读取回调
 * 将电容触摸的原始坐标转换到 LVGL 坐标系。
 * 未做旋转，按 refer/main.c 非旋转映射：X<-rawY，Y<-(V_RES - rawX)
 */
static void example_lvgl_touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
	(void)indev;
	bool pressed = false;
	uint16_t rx = 0, ry = 0;
	if (lcd_touch_read(&pressed, &rx, &ry) == ESP_OK && pressed) {
		/* 原始触摸坐标与屏坐标方向不同，做简单映射与边界保护 */
		if (rx > EXAMPLE_LCD_V_RES) rx = EXAMPLE_LCD_V_RES;
		if (ry > EXAMPLE_LCD_H_RES) ry = EXAMPLE_LCD_H_RES;
		data->state = LV_INDEV_STATE_PRESSED;
		data->point.x = ry;
		data->point.y = (EXAMPLE_LCD_V_RES - rx);
	} else {
		data->state = LV_INDEV_STATE_RELEASED;
	}
}


void lvgl_bsp_init(void) {

  /* 初始化LCD屏幕 */
  lcd_bsp_init();
  /* 初始化触摸 */
  lcd_touch_bsp_init();

	/** 初始化 LVGL 核心与同步资源 */
  lv_init();

  /* 创建刷新完成信号量与 DMA 缓冲 */
  if (!s_flush_done_sem) {
	s_flush_done_sem = xSemaphoreCreateBinary();
	assert(s_flush_done_sem);
  }
  if (!s_trans_buf) {
	s_trans_buf = heap_caps_malloc(LVGL_DMA_BUFF_LEN, MALLOC_CAP_DMA);
	assert(s_trans_buf);
  }

  lv_display_t *display = example_lvgl_create_display_with_buffers();

  example_lvgl_start_tick_timer();

  example_lvgl_register_flush_ready_callback(display);

	/* 注册 LVGL 触摸输入设备 */
	lv_indev_t *touch_indev = lv_indev_create();
	lv_indev_set_type(touch_indev, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb(touch_indev, example_lvgl_touch_read);

  xTaskCreate(example_lvgl_port_task, "LVGL", LVGL_BSP_TASK_STACK_SIZE, NULL, LVGL_BSP_TASK_PRIORITY, NULL);

	/** 简单 UI：居中数字用于验证刷新 */
  _lock_acquire(&lvgl_api_lock);
  lv_obj_t *scr = lv_display_get_screen_active(display);
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  /* 居中数字标签与定时器 */
  center_label = lv_label_create(scr);
  lv_label_set_text_fmt(center_label, "%d", center_counter);
  lv_obj_set_style_text_color(center_label, lv_color_white(), 0);
  lv_obj_align(center_label, LV_ALIGN_CENTER, 0, 0);
  lv_timer_t *ctr_timer = lv_timer_create(center_counter_timer_cb, 1000, NULL);
  (void)ctr_timer;
	/* 创建一个位于底部的增量按钮，点击时数字加 1 */
	inc_btn = lv_btn_create(scr);
	lv_obj_set_size(inc_btn, 100, 48);
	lv_obj_align(inc_btn, LV_ALIGN_BOTTOM_MID, 0, -20);
	inc_btn_label = lv_label_create(inc_btn);
	lv_label_set_text_fmt(inc_btn_label, "%d", inc_count);
	lv_obj_align(inc_btn_label, LV_ALIGN_CENTER, 0, 0);
	lv_obj_add_event_cb(inc_btn, inc_btn_event_cb, LV_EVENT_CLICKED, NULL);
  _lock_release(&lvgl_api_lock);
}
