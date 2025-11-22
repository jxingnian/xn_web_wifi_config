/*
 * @Author: 星年 && jixingnian@gmail.com
 * @Date: 2025-11-22 21:40:00
 * @LastEditors: xingnian jixingnian@gmail.com
 * @LastEditTime: 2025-11-22 23:32:27
 * @FilePath: \xn_web_wifi_config\components\xn_web_wifi_manger\wifi_spiffs\app.js
 * @Description: Web 配网页面的前端逻辑骨架（仅基础事件与占位渲染）
 *
 * 设计原则：
 * 1. 只负责初始化 DOM 与绑定基础事件
 * 2. 不预设后台 HTTP 接口路径
 * 3. 具体业务逻辑在后续按需逐步补充
 */

(function () {
  'use strict';

  /* -------------------- DOM 引用与内部结构 -------------------- */

  /**
   * DOM 缓存表：集中管理页面中需要访问的元素。
   * 这样后续修改 id 或结构时，只需要维护这一处。
   */
  const dom = {
    // 当前 WiFi 状态模块
    statusText: null,
    statusSsid: null,
    statusIp: null,
    statusMode: null,
    statusSignalBar: null,

    // 已保存 WiFi 模块
    savedBody: null,
    savedEmpty: null,

    // 扫描 WiFi 模块
    scanBody: null,
    scanEmpty: null,
    btnScan: null,

    // 连接 WiFi 模块
    formConnect: null,
    inputSsid: null,
    inputPassword: null,
    connectMessage: null,
  };

  var lastStatusState = 0;
  var lastStatusSsid = null;

  /**
   * 初始化 DOM 引用。
   *
   * 仅做 querySelector，不包含任何业务逻辑，
   * 方便在后续按需扩展时保持结构清晰。
   */
  function initDom() {
    dom.statusText = document.getElementById('wifi-status-text');
    dom.statusSsid = document.getElementById('wifi-status-ssid');
    dom.statusIp = document.getElementById('wifi-status-ip');
    dom.statusMode = document.getElementById('wifi-status-mode');
    dom.statusSignalBar = document.querySelector('.signal-bar');

    dom.savedBody = document.getElementById('saved-list');
    dom.savedEmpty = document.getElementById('saved-empty');

    dom.scanBody = document.getElementById('scan-list');
    dom.scanEmpty = document.getElementById('scan-empty');
    dom.btnScan = document.getElementById('btn-scan');

    dom.formConnect = document.getElementById('connect-form');
    dom.inputSsid = document.getElementById('connect-ssid');
    dom.inputPassword = document.getElementById('connect-password');
    dom.connectMessage = document.getElementById('connect-message');
  }

  /* -------------------- 基础渲染与状态 -------------------- */

  /**
   * 渲染一个最小的初始状态。
   *
   * - 当前认为设备尚未连接到任何 WiFi
   * - 已保存 / 扫描列表保持为空
   * - 徽章与信号条使用默认样式
   */
  function renderInitialState() {
    if (dom.statusText) {
      dom.statusText.textContent = '未连接';
    }
    if (dom.statusSsid) {
      dom.statusSsid.textContent = '-';
    }
    if (dom.statusIp) {
      dom.statusIp.textContent = '-';
    }
    if (dom.statusMode && !dom.statusMode.textContent) {
      dom.statusMode.textContent = 'AP+STA';
    }

    if (dom.statusSignalBar) {
      // 0 表示信号最弱（或未连接），仅作为占位
      dom.statusSignalBar.setAttribute('data-level', '0');
    }

    if (dom.savedBody) {
      dom.savedBody.innerHTML = '';
    }
    if (dom.savedEmpty) {
      dom.savedEmpty.style.display = 'block';
    }

    if (dom.scanBody) {
      dom.scanBody.innerHTML = '';
    }
    if (dom.scanEmpty) {
      dom.scanEmpty.style.display = 'block';
    }

    setConnectMessage('');
  }

  /* -------------------- 当前 WiFi 状态：请求与渲染 -------------------- */

  /**
   * 根据 RSSI 粗略映射信号等级：0~3。
   * 等级仅用于控制 signal-bar 的亮度，不追求精确。
   */
  function levelFromRssi(rssi) {
    if (typeof rssi !== 'number') {
      return 0;
    }
    if (rssi >= -60) return 3;
    if (rssi >= -70) return 2;
    if (rssi >= -80) return 1;
    return 0;
  }

  /**
   * 将后端返回的状态数据应用到页面上。
   *
   * @param data 形如 { connected, ssid, ip, rssi } 的对象
   */
  function applyStatus(data) {
    if (!data) {
      return;
    }

    var state = typeof data.state === 'number' ? data.state : 0;
    var connected = !!data.connected && state === 2; // 仅在“已连接”状态下视为连接成功
    var ssid = data.ssid || '-';
    var ip = data.ip || '-';
    var rssi = typeof data.rssi === 'number' ? data.rssi : null;
    var mode = data.mode || null;

    if (dom.statusText) {
      var text = '未连接';
      if (state === 1) {
        text = '正在连接';
      } else if (state === 2) {
        text = '已连接';
      } else if (state === 3) {
        text = '连接失败';
      }

      dom.statusText.textContent = text;

      // 正在连接时显示转圈加载图标
      if (state === 1) {
        dom.statusText.setAttribute('data-loading', 'true');
      } else {
        dom.statusText.removeAttribute('data-loading');
      }
    }
    if (dom.statusSsid) {
      dom.statusSsid.textContent = ssid;
    }
    if (dom.statusIp) {
      dom.statusIp.textContent = ip;
    }

    // 运行模式：仅在后端提供 mode 字段时覆盖默认值
    if (dom.statusMode && mode) {
      dom.statusMode.textContent = mode;
    }

    // 信号条亮度按 RSSI 粗略映射
    if (dom.statusSignalBar) {
      var level = levelFromRssi(rssi);
      dom.statusSignalBar.setAttribute('data-level', String(level));
    }

    var prevState = lastStatusState;
    var prevSsid = lastStatusSsid;

    lastStatusState = state;
    lastStatusSsid = ssid;

    if (state === 2 && (prevState !== 2 || prevSsid !== ssid)) {
      loadSavedList();
    }
  }

  /* -------------------- 已保存 WiFi：渲染与操作 -------------------- */

  /**
   * 将已保存 WiFi 列表渲染到表格中。
   *
   * @param items 形如 [{ index, ssid }, ...] 的数组
   */
  function renderSavedList(items) {
    if (!dom.savedBody) {
      return;
    }

    items = Array.isArray(items) ? items : [];

    dom.savedBody.innerHTML = '';

    if (dom.savedEmpty) {
      dom.savedEmpty.style.display = items.length === 0 ? 'block' : 'none';
    }

    if (items.length === 0) {
      return;
    }

    var rows = [];
    for (var i = 0; i < items.length; i++) {
      var item = items[i] || {};
      var ssid = item.ssid || '-';

      rows.push(
        '<tr>' +
          '<td>' + (i + 1) + '</td>' +
          '<td>' + ssid + '</td>' +
          '<td>' +
            '<button type="button" class="btn btn-primary" data-action="connect-saved" data-ssid="' + ssid + '">连接</button>' +
            ' ' +
            '<button type="button" class="btn" data-action="delete-saved" data-ssid="' + ssid + '">删除</button>' +
          '</td>' +
        '</tr>'
      );
    }

    dom.savedBody.innerHTML = rows.join('');
  }

  /**
   * 从后端加载一次已保存 WiFi 列表。
   */
  function loadSavedList() {
    if (!dom.savedBody || !window.fetch) {
      return;
    }

    fetch('/api/wifi/saved')
      .then(function (res) {
        if (!res.ok) {
          throw new Error('http ' + res.status);
        }
        return res.json();
      })
      .then(function (data) {
        var items = (data && data.items) || [];
        renderSavedList(items);
      })
      .catch(function () {
        // 失败时保持占位提示，不弹出错误
      });
  }

  /**
   * 删除一条已保存 WiFi，按 SSID 标识。
   */
  function deleteSavedWifi(ssid) {
    if (!ssid || !window.fetch) {
      return;
    }

    fetch('/api/wifi/saved/delete?ssid=' + encodeURIComponent(ssid), {
      method: 'POST',
    })
      .then(function (res) {
        if (!res.ok) {
          throw new Error('http ' + res.status);
        }
      })
      .then(function () {
        // 删除成功后重新加载列表
        loadSavedList();
      })
      .catch(function () {
        // 删除失败时暂不提示，保持列表不变
      });
  }

  /* -------------------- 扫描附近 WiFi：请求与渲染 -------------------- */

  /**
   * 将扫描结果渲染到“扫描附近 WiFi”表格中。
   *
   * @param items 形如 [{ ssid, rssi }, ...] 的数组
   */
  function renderScanList(items) {
    if (!dom.scanBody) {
      return;
    }

    items = Array.isArray(items) ? items : [];

    dom.scanBody.innerHTML = '';

    if (dom.scanEmpty) {
      dom.scanEmpty.style.display = items.length === 0 ? 'block' : 'none';
    }

    if (items.length === 0) {
      return;
    }

    var rows = [];
    for (var i = 0; i < items.length; i++) {
      var item = items[i] || {};
      var ssid = item.ssid || '-';
      var rssi = typeof item.rssi === 'number' ? item.rssi : null;

      var signalText = rssi === null ? '-' : (rssi + ' dBm');

      rows.push(
        '<tr data-ssid="' + ssid + '">' +
          '<td>' + ssid + '</td>' +
          '<td>' + signalText + '</td>' +
          '<td>-</td>' +
        '</tr>'
      );
    }

    dom.scanBody.innerHTML = rows.join('');
  }

  /**
   * 发起一次“扫描附近 WiFi”的请求。
   */
  function loadScanList() {
    if (!dom.scanBody || !window.fetch) {
      return;
    }

    // 简单的“正在扫描”提示
    if (dom.scanEmpty) {
      dom.scanEmpty.textContent = '正在扫描...';
      dom.scanEmpty.style.display = 'block';
    }
    dom.scanBody.innerHTML = '';

    fetch('/api/wifi/scan')
      .then(function (res) {
        if (!res.ok) {
          throw new Error('http ' + res.status);
        }
        return res.json();
      })
      .then(function (data) {
        var items = (data && data.items) || [];
        renderScanList(items);
      })
      .catch(function () {
        if (dom.scanEmpty) {
          dom.scanEmpty.textContent = '扫描失败';
          dom.scanEmpty.style.display = 'block';
        }
      });
  }

  /**
   * 连接一条已保存 WiFi，按 SSID 标识。
   *
   * 实际连接过程由后端状态机驱动，这里仅发起一次连接请求。
   */
  function connectSavedWifi(ssid) {
    if (!ssid || !window.fetch) {
      return;
    }

    fetch('/api/wifi/saved/connect?ssid=' + encodeURIComponent(ssid), {
      method: 'POST',
    })
      .then(function (res) {
        if (!res.ok) {
          throw new Error('http ' + res.status);
        }
      })
      .then(function () {
        // 连接请求发起后，依赖上方“当前 WiFi 状态”模块的轮询展示进度
      })
      .catch(function () {
        // 连接失败时暂不弹框，由状态模块展示“连接失败”状态
      });
  }

  /**
   * 从后端查询一次当前 WiFi 状态。
   *
   * - 仅在页面加载完成后调用一次；
   * - 请求失败时保持初始占位状态，不弹出提示。
   */
  function loadStatusOnce() {
    // 与后端约定的状态查询接口路径
    var url = '/api/wifi/status';

    if (!window.fetch) {
      // 旧环境下不强行适配，保持占位即可
      return;
    }

    fetch(url)
      .then(function (res) {
        if (!res.ok) {
          throw new Error('http ' + res.status);
        }
        return res.json();
      })
      .then(function (data) {
        applyStatus(data || {});
      })
      .catch(function () {
        // 失败时保持默认未连接状态
      });
  }

  /**
   * 启动一个简单的轮询：每隔固定时间刷新一次当前 WiFi 状态。
   *
   * 间隔设置为 1 秒，足够跟上状态变化，又不会造成明显压力。
   */
  function startStatusPolling() {
    // 先立即拉取一次，保证页面初始状态尽快变为真实状态
    loadStatusOnce();

    // 之后每 1 秒刷新一次
    setInterval(loadStatusOnce, 1000);
  }

  /**
   * 设置“连接 WiFi”模块下方的提示信息。
   *
   * 当前仅用于展示占位提示，不区分成功 / 失败样式，
   * 后续如需要可以在这里扩展不同状态的样式。
   */
  function setConnectMessage(message) {
    if (!dom.connectMessage) {
      return;
    }
    dom.connectMessage.textContent = message || '';
  }

  /**
   * 发起一次“连接 WiFi”的请求。
   *
   * 由后端根据 SSID/密码保存配置并触发状态机重连，
   * 实际连接进度通过上方“当前 WiFi 状态”模块反映。
   */
  function connectWifi(ssid, password) {
    if (!ssid || !window.fetch) {
      return;
    }

    var url = '/api/wifi/connect?ssid=' + encodeURIComponent(ssid);
    if (password) {
      url += '&password=' + encodeURIComponent(password);
    }

    fetch(url, {
      method: 'POST',
    })
      .then(function (res) {
        if (!res.ok) {
          throw new Error('http ' + res.status);
        }
      })
      .then(function () {
        setConnectMessage('已发起连接请求，请稍候查看上方状态。');
      })
      .catch(function () {
        setConnectMessage('连接请求发送失败');
      });
  }

  /* -------------------- 事件绑定（仅占位） -------------------- */

  /**
   * 绑定基础事件：
   * - 扫描按钮点击
   * - 连接表单提交
   *
   * 当前实现只做占位，不发起任何实际 HTTP 请求，
   * 方便后续在对应回调中按需接入后端接口。
   */
  function bindEvents() {
    // 扫描附近 WiFi
    if (dom.btnScan) {
      dom.btnScan.addEventListener('click', function (event) {
        event.preventDefault();
        loadScanList();
      });
    }

    // 点击扫描结果行：将 SSID 填入“连接 WiFi”表单
    if (dom.scanBody && dom.inputSsid) {
      dom.scanBody.addEventListener('click', function (event) {
        var node = event.target;
        while (node && node !== dom.scanBody && node.tagName !== 'TR') {
          node = node.parentNode;
        }
        if (!node || node === dom.scanBody) {
          return;
        }

        var ssid = node.getAttribute('data-ssid') || '';
        if (!ssid) {
          return;
        }

        dom.inputSsid.value = ssid;
        if (dom.inputPassword) {
          dom.inputPassword.value = '';
        }

        // 自动滚动到“连接 WiFi”表单区域，方便立即发起连接
        if (dom.formConnect && typeof dom.formConnect.scrollIntoView === 'function') {
          dom.formConnect.scrollIntoView({ behavior: 'smooth', block: 'start' });
        }
        dom.inputSsid.focus();
      });
    }

    // 提交连接 WiFi 表单
    if (dom.formConnect) {
      dom.formConnect.addEventListener('submit', function (event) {
        event.preventDefault();

        var ssid = dom.inputSsid ? dom.inputSsid.value.trim() : '';
        var password = dom.inputPassword ? dom.inputPassword.value : '';

        if (!ssid) {
          setConnectMessage('请输入 SSID');
          return;
        }

        connectWifi(ssid, password);
      });
    }

    // 已保存 WiFi 列表中的“连接 / 删除”按钮（事件委托）
    if (dom.savedBody) {
      dom.savedBody.addEventListener('click', function (event) {
        var target = event.target;
        if (!target) {
          return;
        }

        var action = target.getAttribute('data-action');
        if (action === 'delete-saved') {
          var ssid = target.getAttribute('data-ssid') || '';
          if (ssid) {
            deleteSavedWifi(ssid);
          }
        } else if (action === 'connect-saved') {
          var ssid2 = target.getAttribute('data-ssid') || '';
          if (ssid2) {
            connectSavedWifi(ssid2);
          }
        }
      });
    }
  }

  /* -------------------- 启动入口 -------------------- */

  /**
   * 页面就绪时的入口函数：
   * 1. 初始化 DOM
   * 2. 渲染初始状态
   * 3. 绑定基础事件
   */
  function bootstrap() {
    initDom();
    renderInitialState();
    bindEvents();
    startStatusPolling();
    loadSavedList();
    loadScanList();
  }

  document.addEventListener('DOMContentLoaded', bootstrap);
})();

