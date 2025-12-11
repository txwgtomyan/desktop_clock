idf_component_register(
  SRCS "board_bsp.c"
  REQUIRES driver
  PRIV_REQUIRES tca9554_bsp sdcard_bsp rtc_bsp
  PRIV_INCLUDE_DIRS "../../main"  # allow access to board_config.h currently kept in main
  INCLUDE_DIRS "include")
