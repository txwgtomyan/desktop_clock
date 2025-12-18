idf_component_register(
  SRCS "lcd_bsp.c"
  REQUIRES driver esp_lcd
  PRIV_INCLUDE_DIRS "../../main" 
  PRIV_REQUIRES  lvgl__lvgl espressif__esp_lcd_axs15231b
  INCLUDE_DIRS "include")
