idf_component_register(
  SRCS "tca9554_bsp.c"
  REQUIRES driver
  PRIV_REQUIRES espressif__esp_io_expander_tca9554
  PRIV_INCLUDE_DIRS "../../main"  
  INCLUDE_DIRS "include")
