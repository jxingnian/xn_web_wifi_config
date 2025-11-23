/*
 * @Author: 星年 && jixingnian@gmail.com
 * @Date: 2025-11-22 16:24:42
 * @LastEditors: xingnian jixingnian@gmail.com
 * @LastEditTime: 2025-11-23 17:59:02
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
#include "esp_log.h"

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

/* 统一更新状态并通知上层回调（若配置了 wifi_event_cb） */
static void wifi_manage_notify_state(wifi_manage_state_t new_state)
{
    s_wifi_manage_state = new_state;

    if (s_wifi_cfg.wifi_event_cb) {
        s_wifi_cfg.wifi_event_cb(new_state);
    }
}

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
    out->state     = WEB_WIFI_STATUS_STATE_IDLE;
    strncpy(out->ssid, "-", sizeof(out->ssid));
    out->ssid[sizeof(out->ssid) - 1] = '\0';
    strncpy(out->ip, "-", sizeof(out->ip));
    out->ip[sizeof(out->ip) - 1] = '\0';
    strncpy(out->mode, "-", sizeof(out->mode));
    out->mode[sizeof(out->mode) - 1] = '\0';

    /* 根据状态机与连接标志推导当前抽象状态 */
    if (s_wifi_connecting) {
        out->state = WEB_WIFI_STATUS_STATE_CONNECTING;
    } else {
        switch (s_wifi_manage_state) {
        case WIFI_MANAGE_STATE_CONNECTED:
            out->state     = WEB_WIFI_STATUS_STATE_CONNECTED;
            out->connected = true;
            break;

        case WIFI_MANAGE_STATE_CONNECT_FAILED:
            out->state = WEB_WIFI_STATUS_STATE_FAILED;
            break;

        case WIFI_MANAGE_STATE_DISCONNECTED:
        default:
            out->state = WEB_WIFI_STATUS_STATE_IDLE;
            break;
        }
    }

    /* 非“已连接”状态下不再继续读取底层信息 */
    if (out->state != WEB_WIFI_STATUS_STATE_CONNECTED) {
        return ESP_OK;
    }

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

    /* 读取当前 WiFi 模式，并转换为简短字符串 */
    wifi_mode_t wifi_mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&wifi_mode) == ESP_OK) {
        const char *mode_str = "-";

        switch (wifi_mode) {
        case WIFI_MODE_STA:
            mode_str = "STA";
            break;
        case WIFI_MODE_AP:
            mode_str = "AP";
            break;
        case WIFI_MODE_APSTA:
            mode_str = "AP+STA";
            break;
        default:
            break;
        }

        strncpy(out->mode, mode_str, sizeof(out->mode));
        out->mode[sizeof(out->mode) - 1] = '\0';
    }

    return ESP_OK;
}

/* -------------------- Web 回调：已保存 WiFi 列表与删除 -------------------- */
/**
 * @brief 提供给 Web 的“已保存 WiFi 列表”回调
 */
static esp_err_t wifi_manage_get_web_saved_list(web_saved_wifi_info_t *list, size_t *inout_cnt)
{
    if (inout_cnt == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 读取配置允许的最大保存数量，该值也限制存储层条目上限 */
    uint8_t max_num = (s_wifi_cfg.save_wifi_count <= 0)
                          ? 1
                          : (uint8_t)s_wifi_cfg.save_wifi_count;

    if (max_num == 0) {
        *inout_cnt = 0;
        return ESP_OK;
    }

    /* 为避免在栈上分配大数组，这里通过堆申请临时缓冲区 */
    wifi_config_t *configs = (wifi_config_t *)malloc(max_num * sizeof(wifi_config_t));
    if (configs == NULL) {
        *inout_cnt = 0;
        return ESP_ERR_NO_MEM;
    }

    uint8_t   count = 0;
    esp_err_t ret   = wifi_storage_load_all(configs, &count);
    if (ret != ESP_OK) {
        free(configs);
        *inout_cnt = 0;
        return ret;
    }

    if (count > max_num) {
        count = max_num;
    }

    if (list == NULL) {
        /* 仅查询数量，不返回具体内容 */
        *inout_cnt = count;
        free(configs);
        return ESP_OK;
    }

    /* 实际可写入的条目数取决于调用方提供的容量与已有数量 */
    size_t cap = *inout_cnt;
    if (cap == 0) {
        free(configs);
        *inout_cnt = 0;
        return ESP_ERR_INVALID_ARG;
    }
    if (cap > count) {
        cap = count;
    }

    for (size_t i = 0; i < cap; i++) {
        strncpy(list[i].ssid, (const char *)configs[i].sta.ssid, sizeof(list[i].ssid));
        list[i].ssid[sizeof(list[i].ssid) - 1] = '\0';
    }

    free(configs);

    *inout_cnt = cap;
    return ESP_OK;
}

/**
 * @brief 提供给 Web 的“删除已保存 WiFi”回调
 */
static esp_err_t wifi_manage_delete_web_saved(const char *ssid)
{
    return wifi_storage_delete_by_ssid(ssid);
}

/* -------------------- Web 回调：扫描附近 WiFi -------------------- */
/**
 * @brief 提供给 Web 的“扫描附近 WiFi”回调
 *
 * 通过底层 wifi_module_scan 获取一次附近 AP 列表，
 * 并转换为 Web 端展示所需的精简字段。
 */
static esp_err_t wifi_manage_scan_web(web_scan_result_t *list, size_t *inout_cnt)
{
    if (list == NULL || inout_cnt == NULL || *inout_cnt == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "web scan request: max_cnt=%u", (unsigned)(*inout_cnt));

    uint16_t cap = (uint16_t)(*inout_cnt);
    wifi_module_scan_result_t *results =
        (wifi_module_scan_result_t *)malloc(cap * sizeof(wifi_module_scan_result_t));
    if (results == NULL) {
        *inout_cnt = 0;
        return ESP_ERR_NO_MEM;
    }

    uint16_t count = cap;
    esp_err_t ret = wifi_module_scan(results, &count);
    if (ret != ESP_OK) {
        free(results);
        *inout_cnt = 0;
        return ret;
    }

    ESP_LOGI(TAG, "wifi scan done: count=%u", (unsigned)count);

    size_t out_cnt = (size_t)count;
    if (out_cnt > *inout_cnt) {
        out_cnt = *inout_cnt;
    }

    for (size_t i = 0; i < out_cnt; i++) {
        strncpy(list[i].ssid, results[i].ssid, sizeof(list[i].ssid));
        list[i].ssid[sizeof(list[i].ssid) - 1] = '\0';
        list[i].rssi = results[i].rssi;
    }

    free(results);
    *inout_cnt = out_cnt;
    return ESP_OK;
}

/* -------------------- Web 回调：从已保存列表触发连接 -------------------- */
/**
 * @brief 提供给 Web 的“连接已保存 WiFi”回调
 *
 * 实现思路：
 * 1. 在存储列表中找到目标 SSID，并通过 wifi_storage_on_connected()
 *    将其提升为最高优先级；
 * 2. 主动断开当前 STA 连接，让已有状态机在收到“断开”事件后，
 *    按最新的优先级顺序自动重连。
 *
 * 不直接改动状态机内部逻辑，仅通过“调整优先级 + 触发一次断开”
 * 来驱动后续行为，避免与自动重连策略产生直接冲突。
 */
static esp_err_t wifi_manage_connect_web_saved(const char *ssid)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    /* 读取当前已保存列表，确认该 SSID 存在 */
    uint8_t max_num = (s_wifi_cfg.save_wifi_count <= 0)
                          ? 1
                          : (uint8_t)s_wifi_cfg.save_wifi_count;

    wifi_config_t *configs = (wifi_config_t *)malloc(max_num * sizeof(wifi_config_t));
    if (configs == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t   count = 0;
    esp_err_t ret   = wifi_storage_load_all(configs, &count);
    if (ret != ESP_OK || count == 0) {
        free(configs);
        return ret;
    }

    int found_index = -1;
    for (uint8_t i = 0; i < count; i++) {
        if (configs[i].sta.ssid[0] == '\0') {
            continue;
        }
        if (strncmp((const char *)configs[i].sta.ssid,
                    ssid,
                    sizeof(configs[i].sta.ssid)) == 0) {
            found_index = (int)i;
            break;
        }
    }

    if (found_index < 0) {
        free(configs);
        return ESP_ERR_NOT_FOUND;
    }

    /* 通过存储模块将该配置提升为最高优先级 */
    ret = wifi_storage_on_connected(&configs[found_index]);
    free(configs);

    if (ret != ESP_OK) {
        return ret;
    }

    /* 主动断开当前连接，让状态机在后续收到“断开”事件后，
     * 按最新优先级从首选 WiFi 开始重新尝试连接。 */
    (void)esp_wifi_disconnect();

    return ESP_OK;
}

/* -------------------- Web 回调：表单连接 WiFi -------------------- */
/**
 * @brief 提供给 Web 的“表单连接 WiFi”回调
 *
 */
static esp_err_t wifi_manage_connect_web_form(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    
    const char *pwd = (password != NULL && password[0] != '\0') ? password : NULL;

    return wifi_module_connect(ssid, pwd);
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
        wifi_manage_notify_state(WIFI_MANAGE_STATE_CONNECTED);
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
        wifi_manage_notify_state(WIFI_MANAGE_STATE_DISCONNECTED);
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

        /* 为避免在任务栈上分配大数组，这里通过堆申请临时缓冲区 */
        wifi_config_t *list = (wifi_config_t *)malloc(max_num * sizeof(wifi_config_t));
        if (list == NULL) {
            /* 内存不足时保留在断开状态，等待下次循环再尝试 */
            break;
        }

        uint8_t count = 0;

        if (wifi_storage_load_all(list, &count) != ESP_OK || count == 0) {
            /* 没有可用配置，交由上层决定是否启用纯 AP 配网等逻辑 */
            free(list);
            break;
        }

        if (s_wifi_try_index >= count) {
            /* 本轮所有配置均尝试过，仍未连接成功，进入“整轮失败”状态 */
            wifi_manage_notify_state(WIFI_MANAGE_STATE_CONNECT_FAILED);
            s_connect_failed_ts = xTaskGetTickCount();
            s_wifi_try_index    = 0;
            s_wifi_connecting   = false;
            free(list);
            break;
        }

        wifi_config_t *cfg = &list[s_wifi_try_index];
        if (cfg->sta.ssid[0] == '\0') {
            /* 跳过无效 SSID */
            s_wifi_try_index++;
            free(list);
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

        free(list);
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
            wifi_manage_notify_state(WIFI_MANAGE_STATE_DISCONNECTED);
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

        /* 通过回调向 Web 模块暴露当前 WiFi 状态与已保存列表等能力 */
        web_cfg.get_status_cb     = wifi_manage_get_web_status;
        web_cfg.get_saved_list_cb = wifi_manage_get_web_saved_list;
        web_cfg.scan_cb           = wifi_manage_scan_web;
        web_cfg.delete_saved_cb   = wifi_manage_delete_web_saved;
        web_cfg.connect_saved_cb  = wifi_manage_connect_web_saved;
        web_cfg.connect_cb        = wifi_manage_connect_web_form;

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
