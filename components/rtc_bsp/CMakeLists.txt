idf_component_register(
  SRCS "rtc_bsp.c"
  REQUIRES driver
  PRIV_REQUIRES tca9554_bsp waveshare__pcf85063a
  INCLUDE_DIRS "include")
