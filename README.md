<!--
 * @Author: 星年 && jixingnian@gmail.com
 * @Date: 2025-11-22 15:20:30
 * @LastEditors: xingnian jixingnian@gmail.com
 * @LastEditTime: 2025-11-23 12:09:25
 * @FilePath: \xn_web_wifi_config\README.md
 * @Description: ESP32 网页 WiFi 配网组件使用说明
 * 
 * Copyright (c) 2025 by ${git_name_email}, All Rights Reserved. 
-->

# ESP32 网页 WiFi 配网组件（xn_web_wifi_config）

## 1. 项目简介

本仓库提供一个基于 ESP-IDF 的 **ESP32 网页 WiFi 配网组件**，集成了：

- **WiFi 管理状态机**：自动重连、轮询多组已保存 AP；
- **SoftAP + Web 配网页面**：通过浏览器扫描附近 WiFi 并输入密码连接；
- **存储模块**：在 NVS 中保存/删除多组 WiFi；
- **前后端分离的 Web UI**：静态页面放在 SPIFFS 分区，通过 HTTP API 与后台交互。

目标是让应用层只需在 `app_main` 里初始化一次管理模块，就可以快速接入配网能力。

---

## 2. 目录结构

- **main/**
  - `main.c`：应用入口示例（可根据自己项目修改）。

- **components/xn_web_wifi_manger/**
  - **include/**
    - `xn_wifi_manage.h`：WiFi 管理模块对外接口与配置结构。
    - `wifi_module.h`：底层 WiFi 封装接口（init / connect / scan）。
    - `storage_module.h`：保存/加载 WiFi 配置的接口。
    - `web_module.h`：Web 服务器与 HTTP API 接口。
  - **src/**
    - `xn_wifi_manage.c`：WiFi 管理状态机 + 定时任务调度。
    - `wifi_module.c`：对 ESP-IDF `esp_wifi` 的封装（连接、扫描）。
    - `web_module.c`：HTTP 服务器、SPIFFS 静态资源、JSON API。
    - `storage_module.c`：基于 NVS 的 WiFi 配置存储实现。
  - **wifi_spiffs/**
    - `index.html` / `app.css` / `app.js`：Web 配网页面前端资源。

- 根目录：
  - `CMakeLists.txt`：顶层构建脚本。
  - `partitions.csv`：包含 `wifi_spiffs` SPIFFS 分区配置。
  - `sdkconfig.defaults`：默认配置。

---

## 3. 环境要求

- 硬件：ESP32 系列开发板（2.4G WiFi）。
- 软件：已安装配置好的 ESP-IDF。

---

## 4. 编译与烧录

在工程根目录下：

```bash
idf.py set-target esp32-s3      # 如已配置可省略
idf.py build
idf.py flash
idf.py monitor
```

本组件的 `CMakeLists.txt` 中包含：

```cmake
spiffs_create_partition_image(wifi_spiffs wifi_spiffs FLASH_IN_PROJECT)
```

构建时会自动将 `components/xn_web_wifi_manger/wifi_spiffs` 下的静态文件
打包进 `wifi_spiffs` 分区并在 `flash` 时一起烧录，无需单独生成 SPIFFS 镜像。

---

## 5. 在 app_main 中使用示例

典型使用流程如下（示意代码，仅展示核心调用）：

```c
void app_main(void)
{
    // 初始化 NVS / 日志等
    nvs_flash_init();

    // 使用默认配置，并按需覆盖部分字段
    wifi_manage_config_t cfg = WIFI_MANAGE_DEFAULT_CONFIG();

    // 可选：修改 AP SSID/密码/IP/Web 端口 等
    // strcpy(cfg.ap_ssid, "My-ESP32-AP");
    // strcpy(cfg.ap_password, "12345678");
    // strcpy(cfg.ap_ip, "192.168.4.1");
    // cfg.web_port = 80;

    // 可选：设置 WiFi 状态回调，用于上层 UI 或日志
    // cfg.wifi_event_cb = my_wifi_event_cb;

    // 初始化管理模块（内部会初始化 WiFi / 存储 / Web 等子模块）
    esp_err_t ret = wifi_manage_init(&cfg);
    if (ret != ESP_OK) {
        // 处理错误
    }
}
```

上电后管理模块会：

- 初始化 WiFi / 存储 / Web 配网子模块；
- 启动 STA + AP 模式；
- 启动 HTTP 服务器监听 `cfg.web_port`（默认 80）。

---

## 6. Web 配网使用说明

1. **连接 ESP32 的 AP**

   - 默认 AP SSID：`XN-ESP32-AP`
   - 默认密码：`12345678`
   - AP IP：`192.168.4.1`（可通过 `wifi_manage_config_t.ap_ip` 修改）。

2. **浏览器访问**

   在手机或电脑浏览器中输入：

   ```text
   http://192.168.4.1/
   ```

3. **网页功能**

   - 查看当前 WiFi 连接状态（已连接 / 未连接 / 连接失败等）。
   - 点击“开始扫描”扫描附近 2.4G WiFi，并以表格形式展示 SSID 与 RSSI。
   - 点击某一条扫描结果可快速填充 SSID，手动输入密码后提交表单进行连接。
   - 管理已保存 WiFi：查看列表、选择连接、删除已保存的条目。

> 提示：部分安卓手机在检测到当前 WiFi 无互联网时，会自动切到移动数据。
> 如访问 `http://192.168.4.1/` 打不开页面，可暂时关闭移动数据，
> 或在系统弹出的“此网络无互联网访问”提示中选择“仍然使用此网络”。

---

## 7. 组件内部模块概览

### 7.1 WiFi 管理模块（xn_wifi_manage）

- 对外配置结构：`wifi_manage_config_t`（见 `xn_wifi_manage.h`）。
- 重要字段：
  - `ap_ssid` / `ap_password` / `ap_ip`：配网 AP 的 SSID、密码与 IP；
  - `web_port`：Web 配网页面 HTTP 端口；
  - `save_wifi_count`：最多保存的 WiFi 条数；
  - `max_retry_count`：单个 AP 连续重试次数；
  - `reconnect_interval_ms`：整轮失败后多久再自动重试；
  - `wifi_event_cb`：状态变化回调。

内部通过一个周期性运行的任务驱动状态机，周期由
`WIFI_MANAGE_STEP_INTERVAL_MS`（默认 1000ms）控制。

### 7.2 底层 WiFi 模块（wifi_module）

- 主要接口（见 `wifi_module.h`）：
  - `wifi_module_init`：根据配置初始化 ESP32 WiFi（STA/AP/混合模式）。
  - `wifi_module_connect`：连接指定 SSID + 密码。
  - `wifi_module_scan`：扫描附近 AP 并返回结果数组。

管理模块调用这些函数完成具体的连接与扫描动作。

### 7.3 Web 模块（web_module）

负责：

- 挂载 SPIFFS 分区 `wifi_spiffs`；
- 提供静态资源：`/`、`/index.html`、`/app.css`、`/app.js`；
- 提供 JSON API：
  - `GET  /api/wifi/status`
  - `GET  /api/wifi/saved`
  - `GET  /api/wifi/scan`
  - `POST /api/wifi/connect`
  - `POST /api/wifi/saved/delete`
  - `POST /api/wifi/saved/connect`

上层通过回调（在 `web_module_config_t` 中指定）与管理模块/存储模块解耦。

---

## 8. 日志与调试

使用串口监视：

```bash
idf.py monitor
```

主要日志 TAG：

- `wifi_module`：底层 WiFi 连接、扫描结果与错误。  
- `wifi_manage`：管理状态机状态变更（已连接 / 未连接 / 连接失败等）。  
- `web_module`：HTTP 访问、SPIFFS 挂载/读文件失败等。

与 WiFi 扫描相关的日志示例（实际内容以代码为准）：

- `wifi_manage: web scan request: max_cnt=...`
- `wifi_module: start wifi scan, max_out=...`
- `wifi_module: wifi scan done: found N AP(s), out=N`
- `wifi_manage: wifi scan done: count=N`

当网页扫描无结果或崩溃时，可优先查看这些日志定位问题。

---

## 9. 状态机简要说明

WiFi 管理层抽象为三种状态（见 `wifi_manage_state_t`）：

1. `WIFI_MANAGE_STATE_CONNECTED`：已成功连接路由器并获得 IP；
2. `WIFI_MANAGE_STATE_DISCONNECTED`：未连接任何 WiFi；
3. `WIFI_MANAGE_STATE_CONNECT_FAILED`：本轮所有候选 WiFi 均连接失败；

状态机会根据当前状态和事件（连接成功/失败、掉线等）自动选择下一步动作，
例如：

- 从“未连接”尝试连接已保存 WiFi；
- 单个 AP 多次失败后切换到下一条；
- 全部失败后进入“连接失败”状态，等待 `reconnect_interval_ms` 再重新尝试。

应用层可以通过 `wifi_event_cb_t` 回调获知状态变化，用于更新 UI 或执行业务逻辑。

---

## 10. 许可说明

如无特别说明，本组件可按照你项目整体的开源/闭源协议一并分发与使用.
