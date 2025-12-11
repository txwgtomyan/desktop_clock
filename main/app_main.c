/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_bsp.h"

void app_main(void)
{
    printf("hello word");
    board_bsp_init();

    for(int i = 0 ; i <10 ; i++) {
        printf("Restarting in %d seconds...\n", 10 - i);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    fflush(stdout);
    esp_restart();
}
