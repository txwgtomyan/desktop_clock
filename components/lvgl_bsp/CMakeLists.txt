idf_component_register(
  SRCS "lvgl_bsp.c"
  REQUIRES driver esp_lcd esp_timer
  PRIV_INCLUDE_DIRS "../../main" 
  PRIV_REQUIRES  lvgl__lvgl lcd_bsp
  INCLUDE_DIRS "include")
