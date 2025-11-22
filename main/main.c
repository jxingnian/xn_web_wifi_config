/*
 * @Author: 星年 jixingnian@gmail.com
 * @Date: 2025-11-22 13:43:50
 * @LastEditors: xingnian jixingnian@gmail.com
 * @LastEditTime: 2025-11-22 20:17:58
 * @FilePath: \xn_web_wifi_config\main\main.c
 * @Description: esp32 网页WiFi配网 By.星年
 */

#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "xn_wifi_manage.h"

void app_main(void)
{
    printf("esp32 网页WiFi配网 By.星年\n");
    esp_err_t ret = wifi_manage_init(NULL);
    (void)ret; 
}
