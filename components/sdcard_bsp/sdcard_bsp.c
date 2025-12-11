#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "driver/sdmmc_host.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "sdcard_bsp.h"

static const char *TAG = "SDCARD_BSP";
/* 简单演示读写缓冲区上限 */
#define EXAMPLE_MAX_CHAR_SIZE 64

#define MOUNT_POINT "/sdcard"

#define SDMMC_D0_PIN 40
#define SDMMC_CLK_PIN 41
#define SDMMC_CMD_PIN 39

/**
 * @brief 写入演示文件，便于验证挂载是否成功。
 *
 * @param path  目标文件路径。
 * @param data  待写入的字符串数据。
 * @return ESP_OK 写入成功；否则返回错误码。
 */
static esp_err_t s_example_write_file(const char *path, const char *data) {
  ESP_LOGI(TAG, "Opening file %s", path);
  FILE *f = fopen(path, "w");
  if (f == NULL) {
    ESP_LOGE(TAG, "Failed to open file for writing");
    return ESP_FAIL;
  }
  fprintf(f, "%s", data);
  fclose(f);
  ESP_LOGI(TAG, "File written");

  return ESP_OK;
}

/**
 * @brief 读取演示文件，确认读写正常。
 *
 * @param path  目标文件路径。
 * @return ESP_OK 读取成功；否则返回错误码。
 */
static esp_err_t s_example_read_file(const char *path) {
  ESP_LOGI(TAG, "Reading file %s", path);
  FILE *f = fopen(path, "r");
  if (f == NULL) {
    ESP_LOGE(TAG, "Failed to open file for reading");
    return ESP_FAIL;
  }
  char line[EXAMPLE_MAX_CHAR_SIZE];
  fgets(line, sizeof(line), f);
  fclose(f);

  char *pos = strchr(line, '\n');
  if (pos) {
    *pos = '\0';
  }
  ESP_LOGI(TAG, "Read from file: '%s'", line);

  return ESP_OK;
}

/**
 * @brief 初始化 SD 卡：挂载 FAT 文件系统，可选做一次读写自检。
 *
 * @return ESP_OK 成功；否则返回具体错误码。
 */
esp_err_t sdcard_bsp_init(void) {
  esp_err_t ret;
  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = false,
      .max_files = 5,
      .allocation_unit_size = 16 * 1024};

  sdmmc_card_t *card = NULL;
  const char mount_point[] = MOUNT_POINT;

  /* 创建 SDMMC 的默认配置 */
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();

  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  slot_config.width = 1; // 仅用 D0，节省引脚

  /* 根据实物连接修改引脚 */
  slot_config.clk = SDMMC_CLK_PIN;
  slot_config.cmd = SDMMC_CMD_PIN;
  slot_config.d0 = SDMMC_D0_PIN;

  slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  ESP_LOGI(TAG, "Mounting filesystem");
  ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config,
                                &card);

  if (ret != ESP_OK) {
    if (ret == ESP_FAIL) {
      ESP_LOGE(TAG, "Failed to mount filesystem. "
                    "If you want the card to be formatted, set the "
                    "EXAMPLE_FORMAT_IF_MOUNT_FAILED menuconfig option.");
    } else {
      ESP_LOGE(TAG,
               "Failed to initialize the card (%s). "
               "Make sure SD card lines have pull-up resistors in place.",
               esp_err_to_name(ret));
    }
    return ret;
  }

  ESP_LOGI(TAG, "Filesystem mounted");
  sdmmc_card_print_info(stdout, card);

  /* 可选：做一次读写自检 */
  const char *test_file = MOUNT_POINT "/hello.txt";
  if (s_example_write_file(test_file, "Hello SD card!")) {
    ESP_LOGW(TAG, "Write test failed");
  } else {
    s_example_read_file(test_file);
  }

  return ESP_OK;
}