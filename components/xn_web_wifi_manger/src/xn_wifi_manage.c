/*
 * @Author: 星年 && jixingnian@gmail.com
 * @Date: 2025-11-22 16:24:42
 * @LastEditors: xingnian jixingnian@gmail.com
 * @LastEditTime: 2025-11-22 21:30:38
 * @FilePath: \xn_web_wifi_config\components\xn_web_wifi_manger\src\xn_wifi_manage.c
 * @Description: WiFi 管理模块实现（封装 WiFi / 存储 / Web 配网，提供自动重连与状态管理）
 */

#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "esp_netif.h"

#include "wifi_module.h"
#include "storage_module.h"
#include "web_module.h"
#include "xn_wifi_manage.h"

/* 日志 TAG（如需日志输出，使用 ESP_LOGx(TAG, ...)） */
static const char *TAG = "wifi_manage";

/* 当前 WiFi 管理状态 */
static wifi_manage_state_t  s_wifi_manage_state = WIFI_MANAGE_STATE_DISCONNECTED;
/* 上层传入的管理配置（保存 WiFi 数量、重连间隔、AP 信息等） */
static wifi_manage_config_t s_wifi_cfg;
/* WiFi 管理任务句柄 */
static TaskHandle_t         s_wifi_manage_task  = NULL;

/* 遍历已保存 WiFi 时的状态 */
static bool       s_wifi_connecting   = false;  /* 当前是否有一次 STA 连接正在进行 */
static uint8_t    s_wifi_try_index    = 0;      /* 本轮遍历中，正在尝试的 WiFi 下标 */
static TickType_t s_connect_failed_ts = 0;      /* 最近一次全轮尝试失败的时间戳 */

/* -------------------- Web 回调：查询当前 WiFi 状态 -------------------- */
/**
 * @brief 提供给 Web 模块的 WiFi 状态查询回调
 *
 * 仅返回网页展示所需的少量字段：
 * - 是否已连接；
 * - 当前 SSID；
 * - 当前 IPv4 地址；
 * - 当前 RSSI。
 */
static esp_err_t wifi_manage_get_web_status(web_wifi_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 统一设置默认值，避免调用方看到未初始化字段 */
    memset(out, 0, sizeof(*out));
    out->connected = false;
    strncpy(out->ssid, "-", sizeof(out->ssid));
    out->ssid[sizeof(out->ssid) - 1] = '\0';
    strncpy(out->ip, "-", sizeof(out->ip));
    out->ip[sizeof(out->ip) - 1] = '\0';

    /* 如管理状态机认为“未连接”，直接返回默认占位值 */
    if (s_wifi_manage_state != WIFI_MANAGE_STATE_CONNECTED) {
        return ESP_OK;
    }

    out->connected = true;

    /* 读取当前连接 AP 的基础信息（SSID + RSSI） */
    wifi_ap_record_t ap_info = {0};
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        strncpy(out->ssid, (const char *)ap_info.ssid, sizeof(out->ssid));
        out->ssid[sizeof(out->ssid) - 1] = '\0';
        out->rssi = ap_info.rssi;
    }

    /* 读取当前 STA IPv4 地址（根据默认 netif 关键字获取） */
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif != NULL) {
        esp_netif_ip_info_t ip_info = {0};
        if (esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK) {
            /* 使用 IPSTR / IP2STR 宏将 IPv4 地址转换为文本 */
            snprintf(out->ip,
                     sizeof(out->ip),
                     IPSTR,
                     IP2STR(&ip_info.ip));
        }
    }

    return ESP_OK;
}

/* -------------------- WiFi 模块事件回调 -------------------- */
/**
 * @brief 供 WiFi 模块调用的事件回调，用于驱动管理状态机
 */
static void wifi_manage_on_wifi_event(wifi_module_event_t event)
{
    switch (event) {
    case WIFI_MODULE_EVENT_STA_CONNECTED:
        /* 已与 AP 建立物理连接，但可能尚未获取 IP，此处仅作占位 */
        break;

    case WIFI_MODULE_EVENT_STA_GOT_IP: {
        /* 获取到 IP，认为一次连接流程成功结束 */
        s_wifi_manage_state = WIFI_MANAGE_STATE_CONNECTED;
        s_wifi_connecting   = false;
        s_wifi_try_index    = 0;      /* 下次自动重连从首选 WiFi 开始 */
        s_connect_failed_ts = 0;

        /* 将当前配置上报给存储模块，用于调整优先级等策略 */
        wifi_config_t current_cfg = {0};
        if (esp_wifi_get_config(WIFI_IF_STA, &current_cfg) == ESP_OK) {
            (void)wifi_storage_on_connected(&current_cfg);
        }
        break;
    }

    case WIFI_MODULE_EVENT_STA_DISCONNECTED:
        /* 连接断开，等待管理任务按策略进行重连 */
        s_wifi_manage_state = WIFI_MANAGE_STATE_DISCONNECTED;
        s_wifi_connecting   = false;
        s_wifi_try_index    = 0;
        break;

    case WIFI_MODULE_EVENT_STA_CONNECT_FAILED:
        /* 本次尝试失败，简单移动到下一条配置 */
        s_wifi_connecting = false;
        s_wifi_try_index++;
        break;

    default:
        /* 其他事件暂不关心 */
        break;
    }
}

/* -------------------- 状态机核心逻辑 -------------------- */
/**
 * @brief 单步执行 WiFi 管理状态机
 *
 * 按当前状态决定是否发起连接、切换状态或等待重试。
 */
static void wifi_manage_step(void)
{
    switch (s_wifi_manage_state) {
    case WIFI_MANAGE_STATE_DISCONNECTED: {
        /* 断开状态：按顺序遍历已保存 WiFi，逐个尝试连接 */

        if (s_wifi_connecting) {
            /* 已经有一个连接操作在进行，等待事件回调给结果 */
            break;
        }

        /* 从存储模块加载全部配置，数量受 save_wifi_count 限制 */
        uint8_t max_num = (s_wifi_cfg.save_wifi_count <= 0)
                              ? 1
                              : (uint8_t)s_wifi_cfg.save_wifi_count;

        wifi_config_t list[max_num];
        uint8_t       count = 0;

        if (wifi_storage_load_all(list, &count) != ESP_OK || count == 0) {
            /* 没有可用配置，交由上层决定是否启用纯 AP 配网等逻辑 */
            break;
        }

        if (s_wifi_try_index >= count) {
            /* 本轮所有配置均尝试过，仍未连接成功，进入“整轮失败”状态 */
            s_wifi_manage_state = WIFI_MANAGE_STATE_CONNECT_FAILED;
            s_connect_failed_ts = xTaskGetTickCount();
            s_wifi_try_index    = 0;
            s_wifi_connecting   = false;
            break;
        }

        wifi_config_t *cfg = &list[s_wifi_try_index];
        if (cfg->sta.ssid[0] == '\0') {
            /* 跳过无效 SSID */
            s_wifi_try_index++;
            break;
        }

        const char *ssid     = (const char *)cfg->sta.ssid;
        const char *password = (cfg->sta.password[0] == '\0')
                                   ? NULL
                                   : (const char *)cfg->sta.password;

        /* 尝试发起连接，成功则等待事件回调，失败则立即切换到下一条 */
        if (wifi_module_connect(ssid, password) == ESP_OK) {
            s_wifi_connecting = true;
        } else {
            s_wifi_try_index++;
        }
        break;
    }

    case WIFI_MANAGE_STATE_CONNECTED:
        /* 已连接状态下，当前不做周期性操作，保持静默 */
        break;

    case WIFI_MANAGE_STATE_CONNECT_FAILED: {
        /* 一轮全部失败，根据配置的重连间隔决定何时重新遍历 */

        if (s_wifi_cfg.reconnect_interval_ms < 0) {
            /* 小于 0 表示关闭自动重连，保持在失败状态 */
            break;
        }

        TickType_t now   = xTaskGetTickCount();
        TickType_t delta = now - s_connect_failed_ts;
        TickType_t need  = pdMS_TO_TICKS((s_wifi_cfg.reconnect_interval_ms <= 0)
                                             ? 0
                                             : s_wifi_cfg.reconnect_interval_ms);

        if (delta >= need) {
            /* 到达重试时间，从头开始新一轮遍历 */
            s_wifi_try_index    = 0;
            s_wifi_connecting   = false;
            s_wifi_manage_state = WIFI_MANAGE_STATE_DISCONNECTED;
        }
        break;
    }

    default:
        /* 理论上不应到达，保留作防护 */
        break;
    }
}

/* -------------------- WiFi 管理任务 -------------------- */
/**
 * @brief 管理任务：周期性驱动状态机运行
 */
static void wifi_manage_task(void *arg)
{
    (void)arg;

    for (;;) {
        wifi_manage_step();
        vTaskDelay(pdMS_TO_TICKS(WIFI_MANAGE_STEP_INTERVAL_MS));
    }
}

/* -------------------- 管理模块初始化 -------------------- */
/**
 * @brief  WiFi 管理模块初始化入口
 *
 * 负责：
 * 1. 保存并标准化上层配置
 * 2. 初始化 WiFi 模块（STA+AP）
 * 3. 初始化存储模块（保存常用 WiFi）
 * 4. 初始化 Web 配网模块（HTTP 服务与回调）
 * 5. 创建管理任务，启动状态机
 */
esp_err_t wifi_manage_init(const wifi_manage_config_t *config)
{
    /* 使用默认配置或上层传入配置 */
    if (config == NULL) {
        s_wifi_cfg = WIFI_MANAGE_DEFAULT_CONFIG();
    } else {
        s_wifi_cfg = *config;
    }

    /* ---- 初始化 WiFi 模块 ---- */
    wifi_module_config_t wifi_cfg = WIFI_MODULE_DEFAULT_CONFIG();

    /* 管理模块要求同时启用 STA + AP：
     * - STA 负责连接路由器上网
     * - AP 负责本地配网访问
     */
    wifi_cfg.enable_sta = true;
    wifi_cfg.enable_ap  = true;

    /* 配网 AP SSID */
    strncpy(wifi_cfg.ap_ssid, s_wifi_cfg.ap_ssid, sizeof(wifi_cfg.ap_ssid));
    wifi_cfg.ap_ssid[sizeof(wifi_cfg.ap_ssid) - 1] = '\0';

    /* 配网 AP 密码 */
    strncpy(wifi_cfg.ap_password, s_wifi_cfg.ap_password, sizeof(wifi_cfg.ap_password));
    wifi_cfg.ap_password[sizeof(wifi_cfg.ap_password) - 1] = '\0';

    /* 配网 AP IP 地址 */
    strncpy(wifi_cfg.ap_ip, s_wifi_cfg.ap_ip, sizeof(wifi_cfg.ap_ip));
    wifi_cfg.ap_ip[sizeof(wifi_cfg.ap_ip) - 1] = '\0';

    /* 绑定 WiFi 事件回调 */
    wifi_cfg.event_cb = wifi_manage_on_wifi_event;

    /* 初始化底层 WiFi 模块 */
    esp_err_t ret = wifi_module_init(&wifi_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    /* ---- 初始化存储模块 ---- */
    wifi_storage_config_t storage_cfg = WIFI_STORAGE_DEFAULT_CONFIG();

    /* 保存 WiFi 数量下限为 1，避免 0 导致逻辑异常 */
    if (s_wifi_cfg.save_wifi_count <= 0) {
        storage_cfg.max_wifi_num = 1;
    } else {
        storage_cfg.max_wifi_num = (uint8_t)s_wifi_cfg.save_wifi_count;
    }

    ret = wifi_storage_init(&storage_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    /* ---- 初始化 Web 配网模块 ---- */
    {
        web_module_config_t web_cfg = WEB_MODULE_DEFAULT_CONFIG();

        /* 端口由管理配置决定，<=0 时沿用默认值 */
        if (s_wifi_cfg.web_port > 0) {
            web_cfg.http_port = s_wifi_cfg.web_port;
        }

        /* 通过回调向 Web 模块暴露当前 WiFi 状态查询能力 */
        web_cfg.get_status_cb = wifi_manage_get_web_status;

        ret = web_module_init(&web_cfg);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    // 创建WiFi管理任务
    if (s_wifi_manage_task == NULL) {
        BaseType_t ret_task = xTaskCreate(
            wifi_manage_task,
            "wifi_manage",
            4096,               // 任务栈大小，可根据实际需要调整
            NULL,
            tskIDLE_PRIORITY + 1,   // 任务优先级，可根据实际需要调整
            &s_wifi_manage_task);

        if (ret_task != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}
