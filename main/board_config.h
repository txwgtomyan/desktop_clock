#ifndef __BOARD_CONFIG_H_
#define __BOARD_CONFIG_H_

#define MAX_I2C_BUS_NUM (2)

#define I2C_MASTER_SCL_IO (GPIO_NUM_48)
#define I2C_MASTER_SDA_IO (GPIO_NUM_47)

#define Touch_SCL_NUM (GPIO_NUM_18)
#define Touch_SDA_NUM (GPIO_NUM_17)

#define DISP_TOUCH_ADDR               (0x3B)

#define EXAMPLE_LCD_H_RES              172
#define EXAMPLE_LCD_V_RES              640
#define LVGL_DMA_BUFF_LEN    (EXAMPLE_LCD_H_RES * 64 * 2)
#define LVGL_SPIRAM_BUFF_LEN (EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * 2)

#define EXAMPLE_PIN_NUM_LCD_CS            (GPIO_NUM_9)
#define EXAMPLE_PIN_NUM_LCD_PCLK          (GPIO_NUM_10) 
#define EXAMPLE_PIN_NUM_LCD_DATA0         (GPIO_NUM_11)
#define EXAMPLE_PIN_NUM_LCD_DATA1         (GPIO_NUM_12)
#define EXAMPLE_PIN_NUM_LCD_DATA2         (GPIO_NUM_13)
#define EXAMPLE_PIN_NUM_LCD_DATA3         (GPIO_NUM_14)
#define EXAMPLE_PIN_NUM_LCD_RST           (GPIO_NUM_21)
#define EXAMPLE_PIN_NUM_BK_LIGHT          (GPIO_NUM_8)

#endif
