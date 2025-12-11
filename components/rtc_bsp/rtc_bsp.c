#include "rtc_bsp.h"
#include "esp_check.h"
#include "esp_log.h"
#include "pcf85063a.h"
#include "driver/i2c_master.h"
#include <string.h>

static const char *TAG = "RTC_BSP";

static pcf85063a_dev_t s_rtc_dev;

esp_err_t rtc_bsp_init(void)
{
    esp_err_t ret = ESP_OK;
    i2c_master_bus_handle_t i2c_bus = NULL;

    ESP_RETURN_ON_ERROR(i2c_master_get_bus_handle(I2C_NUM_0, &i2c_bus), TAG, "get I2C bus failed");

    ret = pcf85063a_init(&s_rtc_dev, i2c_bus, PCF85063A_ADDRESS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "pcf85063a_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGD(TAG, "RTC initialized");

    /* 尝试读取当前时间并打印（非必须） */
    pcf85063a_datetime_t now;
    char datetime_str[64] = {0};
    ret = pcf85063a_get_time_date(&s_rtc_dev, &now);
    if (ret == ESP_OK) {
        pcf85063a_datetime_to_str(datetime_str, now);
        ESP_LOGI(TAG, "Now_time is %s", datetime_str);
    } else {
        ESP_LOGW(TAG, "pcf85063a_get_time_date failed: %s", esp_err_to_name(ret));
    }

    return ESP_OK;
}