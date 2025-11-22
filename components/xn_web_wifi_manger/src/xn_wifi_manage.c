/*
 * @Author: 星年 && jixingnian@gmail.com
 * @Date: 2025-11-22 16:24:42
 * @LastEditors: xingnian jixingnian@gmail.com
 * @LastEditTime: 2025-11-22 18:18:38
 * @FilePath: \xn_web_wifi_config\components\xn_web_wifi_manger\src\xn_wifi_manage.c
 * @Description: WiFi 管理模块实现。
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

#include "wifi_module.h"
#include "xn_wifi_manage.h"

/* 日志 TAG，用于 ESP_LOGx 系列接口（如果后续需要打印日志，可直接使用此 TAG） */
static const char *TAG = "xn_wifi_manage";

/* WiFi 管理状态机当前状态。 */
static wifi_manage_state_t s_wifi_manage_state = WIFI_MANAGE_STATE_DISCONNECTED;
static wifi_manage_config_t s_wifi_cfg;
static TimerHandle_t s_wifi_manage_timer = NULL;

/**
 * @brief WiFi 模块事件回调
 * 
 */
static void wifi_manage_on_wifi_event(wifi_module_event_t event)
{
    switch (event) {
    case WIFI_MODULE_EVENT_STA_CONNECTED:
        /* WiFi 模块上报：STA 与 AP 建立物理连接（未必已经获取到 IP）
         */
        break;

    case WIFI_MODULE_EVENT_STA_GOT_IP:
        /* WiFi 模块上报：STA 已获取到 IP，认为 WiFi 连接完全成功
         */
        s_wifi_manage_state = WIFI_MANAGE_STATE_CONNECTED;
        break;

    case WIFI_MODULE_EVENT_STA_DISCONNECTED:
        /* WiFi 模块上报：STA 与 AP 连接断开
         */
        s_wifi_manage_state = WIFI_MANAGE_STATE_DISCONNECTED;
        break;

    case WIFI_MODULE_EVENT_STA_CONNECT_FAILED:
        /* WiFi 模块上报：本次 STA 连接 AP 过程失败
         */
        s_wifi_manage_state = WIFI_MANAGE_STATE_CONNECT_FAILED;
        break;

    default:
        /* 未关心的事件类型，暂不做处理 */
        break;
    }
}

/**
 * @brief WiFi 管理状态机
 *
 */
static void xn_wifi_manage_step(void)
{
    /* 根据当前记录的状态，选择相应的处理分支 */
    switch (s_wifi_manage_state) {
    case WIFI_MANAGE_STATE_DISCONNECTED:
        /* 状态：WIFI_MANAGE_STATE_DISCONNECTED（WiFi 已断开/尚未连接）
         * 遍历已保存的WiFi配置，尝试连接
         */
        break;

    case WIFI_MANAGE_STATE_CONNECTED:
        /* 状态：WIFI_MANAGE_STATE_CONNECTED（WiFi 已成功连接）
         *
         */
        break;

    case WIFI_MANAGE_STATE_CONNECT_FAILED:
        /* 状态：WIFI_MANAGE_STATE_CONNECT_FAILED（WiFi 连接失败）
         *
         */
        break;

    default:
        /* 理论上不应到达此分支，保留用于兜底保护或调试。 */
        break;
    }
}

static void wifi_manage_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    xn_wifi_manage_step();
}

/**
 * @brief  WiFi 管理模块初始化
 *
 * @param config  用户传入的 WiFi 管理配置（可为 NULL，此时使用默认值）
 * @return esp_err_t
 *         - ESP_OK        : 初始化成功
 *         - ESP_ERR_NO_MEM: 创建重试定时器失败
 */
esp_err_t xn_wifi_manage_init(const wifi_manage_config_t *config)
{
    if (config == NULL) {
        s_wifi_cfg = WIFI_MANAGE_DEFAULT_CONFIG();
    } else {
        s_wifi_cfg = *config;
    }

    // 初始化 WiFi 模块配置，使用模块提供的默认配置作为基础
    wifi_module_config_t wifi_cfg = WIFI_MODULE_DEFAULT_CONFIG();

    // 根据当前 WiFi 管理配置，决定是否启用 STA / AP 模式
    wifi_cfg.enable_sta = true;  // 始终启用 STA，用于连接路由器上网
    wifi_cfg.enable_ap  = true;  // 始终启用 AP，用于配网或本地访问

    // 拷贝配网 AP 的 SSID 到 WiFi 模块配置
    // 使用 strncpy 避免缓冲区溢出，并手动添加字符串结束符
    strncpy(wifi_cfg.ap_ssid, s_wifi_cfg.ap_ssid, sizeof(wifi_cfg.ap_ssid));
    wifi_cfg.ap_ssid[sizeof(wifi_cfg.ap_ssid) - 1] = '\0';

    // 拷贝配网 AP 的密码到 WiFi 模块配置
    // 同样使用安全拷贝，并确保字符串以 '\0' 结尾
    strncpy(wifi_cfg.ap_password, s_wifi_cfg.ap_password, sizeof(wifi_cfg.ap_password));
    wifi_cfg.ap_password[sizeof(wifi_cfg.ap_password) - 1] = '\0';

    // 将 WiFi 模块事件回调指向当前管理模块，用于在获取到 IP/断开等事件时更新状态机
    wifi_cfg.event_cb = wifi_manage_on_wifi_event;

    // 调用底层 WiFi 模块初始化函数
    // 若初始化失败，则直接返回错误码给上层处理
    esp_err_t ret = xn_wifi_module_init(&wifi_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    // 初始化WiFi存储模块
    // 初始化WEB配网模块

    if (s_wifi_manage_timer == NULL) {
        s_wifi_manage_timer = xTimerCreate(
            "wifi_manage",
            pdMS_TO_TICKS(WIFI_MANAGE_STEP_INTERVAL_MS),
            pdTRUE,
            NULL,
            wifi_manage_timer_cb);
    }

    if (s_wifi_manage_timer != NULL) {
        xTimerStart(s_wifi_manage_timer, 0);
    }

    return ESP_OK;
}
