#include <assert.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "driver/spi_master.h"

#include "board_config.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "lcd_bsp.h"
#include "lvgl.h"
#include "lvgl_bsp.h"
#include "sys/lock.h"

static const char *TAG = "lvgl_bsp";

extern esp_lcd_panel_handle_t s_panel;
extern esp_lcd_panel_io_handle_t s_panel_io;

#define LVGL_BSP_TICK_PERIOD_MS        2
#define LVGL_BSP_TASK_MAX_DELAY_MS     500
#define LVGL_BSP_TASK_MIN_DELAY_MS     (1000 / CONFIG_FREERTOS_HZ)
#define LVGL_BSP_TASK_STACK_SIZE       (8 * 1024)
#define LVGL_BSP_TASK_PRIORITY         2

#define LVGL_BSP_MIN(A, B)             ((A) < (B) ? (A) : (B))
#define LVGL_BSP_MAX(A, B)             ((A) > (B) ? (A) : (B))

/* 刷新完成同步信号量与 DMA 传输缓冲 */
static SemaphoreHandle_t s_flush_done_sem = NULL;
static uint16_t *s_trans_buf = NULL; /* MALLOC_CAP_DMA 缓冲，用于分块传输 */
static uint16_t *s_lvgl_fb = NULL;   /* 整屏帧缓冲（SPIRAM） */

/* 面板兼容性开关：端点语义与字节序 */
#ifndef PANEL_END_INCLUSIVE
#define PANEL_END_INCLUSIVE 0 /* 0: [x1,x2)  1: [x1,x2] */
#endif
#ifndef PANEL_NEEDS_SWAP
#define PANEL_NEEDS_SWAP 0 /* 1: 需要 RGB565 字节交换；0: 不交换 */
#endif

/* 居中数字用于验证刷新 */
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

static void example_lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
	esp_lcd_panel_handle_t panel_handle = lv_display_get_user_data(disp);
	/* 参考 refer：忽略局部区域，整屏传输，避免部分刷新不兼容 */
	int offsetx1 = 0;
	int offsetx2 = EXAMPLE_LCD_H_RES;
	int offsety1 = 0;
	int offsety2 = EXAMPLE_LCD_V_RES;

	int width = EXAMPLE_LCD_H_RES;
	int height = EXAMPLE_LCD_V_RES;
	/* 计算一次可传的最大行数（按 DMA 缓冲长度） */
	const size_t pixels_per_chunk = LVGL_DMA_BUFF_LEN / sizeof(uint16_t);
	int max_lines = (int)(pixels_per_chunk / LVGL_BSP_MAX(width, 1));
	if (max_lines < 1) {
		max_lines = 1;
	}

	const uint16_t *src = (const uint16_t *)px_map; /* 等同于 s_lvgl_fb */
	int sent_lines = 0;
	while (sent_lines < height) {
		int lines = LVGL_BSP_MIN(max_lines, height - sent_lines);
		size_t copy_pixels = (size_t)width * lines;

		/* 拷贝当前分块到 DMA 缓冲，并进行 RGB565 字节序交换 */
		memcpy(s_trans_buf, src + (size_t)sent_lines * width, copy_pixels * sizeof(uint16_t));
		if (PANEL_NEEDS_SWAP) {
			lv_draw_sw_rgb565_swap((uint8_t *)s_trans_buf, copy_pixels);
		}

		int y1 = offsety1 + sent_lines;
		/* 结束坐标：可配置为包含或不包含端 */
#if PANEL_END_INCLUSIVE
		int y2 = y1 + lines - 1;
		int x2_end = offsetx1 + width - 1;
#else
		int y2 = y1 + lines;
		int x2_end = offsetx1 + width;
#endif

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
	ESP_LOGW(TAG, "full flush: %dx%d", width, height);
}

static bool example_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
	/* 由 ISR 通知传输完成，flush 回调中等待该信号后再调用 flush_ready */
	BaseType_t awoken = pdFALSE;
	if (s_flush_done_sem) {
		xSemaphoreGiveFromISR(s_flush_done_sem, &awoken);
	}
	ESP_EARLY_LOGI(TAG, "on_color_trans_done");
	return false;
}

static void example_increase_lvgl_tick(void *arg)
{
	/* 通知 LVGL 经过了多少毫秒 */
	lv_tick_inc(LVGL_BSP_TICK_PERIOD_MS);
}

static _lock_t lvgl_api_lock;

static void example_lvgl_port_task(void *arg)
{
	ESP_LOGI(TAG, "Starting LVGL task");
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

static void example_lvgl_register_flush_ready_callback(lv_display_t *display)
{
	const esp_lcd_panel_io_callbacks_t cbs = {
		.on_color_trans_done = example_notify_lvgl_flush_ready,
	};
	ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(s_panel_io, &cbs, display));
}


void lvgl_bsp_init(void) {

  /* 初始化LCD屏幕 */
  lcd_bsp_init();
  /* 初始化触摸 */
  lcd_touch_bsp_init();

  ESP_LOGI(TAG, "lvgl bsp init finished");
	/* 提升日志等级，便于观察回调与刷新链路 */
	esp_log_level_set(TAG, ESP_LOG_DEBUG);
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

  ESP_LOGI(TAG, "Install LVGL tick timer");
  example_lvgl_start_tick_timer();

  ESP_LOGI(TAG, "Register io panel event callback for LVGL flush ready notification");
	ESP_LOGI(TAG, "panel_io handle=%p", s_panel_io);
  example_lvgl_register_flush_ready_callback(display);

  ESP_LOGI(TAG, "Create LVGL task");
  xTaskCreate(example_lvgl_port_task, "LVGL", LVGL_BSP_TASK_STACK_SIZE, NULL, LVGL_BSP_TASK_PRIORITY, NULL);

  /* 简单 UI：居中数字用于验证刷新 */
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
  _lock_release(&lvgl_api_lock);
}
