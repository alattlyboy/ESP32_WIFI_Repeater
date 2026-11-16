/* ESP32 双频中继器 · 前端 2.1
 * 无框架、无构建，直接编进固件。 */
const $ = (id) => document.getElementById(id);

let cfgLoaded = false;
let lastHist = [];
let scanCacheAt = 0;      /* 本地扫描缓存时间，10 秒内重复点直接复用 */

/* ---------- 工具 ---------- */

function fmtDur(s) {
  s = Math.floor(s || 0);
  if (s < 60) return s + "s";
  if (s < 3600) return Math.floor(s / 60) + "m";
  const h = Math.floor(s / 3600);
  if (h < 24) return h + "h" + (Math.floor(s % 3600 / 60)) + "m";
  return Math.floor(h / 24) + "d" + (h % 24) + "h";
}

function fmtHeap(b) {
  if (!b) return "--";
  return Math.round(b / 1024) + "K";
}

/* 带超时的 fetch：早先固件未配置上行时 /api/status 会挂住，
 * 浏览器默认要等 300 秒，页面就永远停在“读取中”。 */
async function jfetch(url, opts) {
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), 4000);
  try {
    const r = await fetch(url, Object.assign({ signal: ctl.signal, cache: "no-store" }, opts || {}));
    return await r.json();
  } finally {
    clearTimeout(timer);
  }
}

/* 802.11 断开原因 → 人话。只列常见的那几个，其余给编号让用户能搜。 */
function reasonText(code) {
  const m = {
    1: "未指定原因",
    2: "认证过期（信号太弱或 AP 踢人）",
    15: "四次握手超时：密码错了，或 AP 要求 WPA2/WPA3 企业版",
    17: "AP 拒绝了关联",
    200: "信标超时（信号太弱 / AP 关机了）",
    201: "找不到这个 WiFi（名称拼写 / 频段不对）",
    202: "认证失败（密码错 / 802.1X 用户名密码错）",
    203: "关联失败（AP 满了或拒绝）",
    204: "握手超时",
    205: "连接失败",
    210: "这个网络需要企业版认证（802.1X），请在下面切换认证方式",
    211: "认证方式不匹配：开了企业版却没填 EAP 信息",
    212: "信号低于设定阈值",
  };
  return m[code] || ("原因码 " + code);
}

/* ---------- 信号可视化 ---------- */

function setBars(rssi) {
  const bars = $("bars").children;
  let n = 0;
  if (rssi <= -80) n = 1;
  else if (rssi <= -70) n = 2;
  else if (rssi <= -60) n = 3;
  else if (rssi < 0) n = 4;
  const tone = n >= 3 ? "on" : (n === 2 ? "on warn" : "on err");
  for (let i = 0; i < bars.length; i++) {
    bars[i].className = i < n ? tone : "";
  }
}

function drawSpark(hist) {
  const cv = $("spark");
  const w = cv.clientWidth;
  if (!w) return;
  const dpr = window.devicePixelRatio || 1;
  const h = 64;
  if (cv.width !== Math.round(w * dpr)) {
    cv.width = Math.round(w * dpr);
    cv.height = Math.round(h * dpr);
  }
  const ctx = cv.getContext("2d");
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);

  const css = getComputedStyle(document.body);
  const line = css.getPropertyValue("--line").trim() || "#252b37";
  const ok = css.getPropertyValue("--ok").trim() || "#35d399";
  const mut = css.getPropertyValue("--mut").trim() || "#8d96a8";

  /* 三条参考线：-40 / -60 / -80 dBm */
  ctx.strokeStyle = line;
  ctx.lineWidth = 1;
  [-40, -60, -80].forEach((v) => {
    const y = 4 + (v + 95) / 70 * (h - 8);
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(w, y);
    ctx.stroke();
  });

  const n = hist.length;
  if (n < 2) {
    ctx.fillStyle = mut;
    ctx.font = "11px sans-serif";
    ctx.fillText("等待信号数据…", 6, h / 2);
    return;
  }

  const px = (i) => (n === 1 ? 0 : (i / (n - 1)) * (w - 4) + 2);
  const py = (v) => 4 + (Math.min(-25, Math.max(-95, v)) + 95) / 70 * (h - 8);

  /* 断线处留空，不要连成一条假线 */
  let started = false;
  ctx.strokeStyle = ok;
  ctx.lineWidth = 2;
  ctx.lineJoin = "round";
  ctx.beginPath();
  for (let i = 0; i < n; i++) {
    const v = hist[i];
    if (!v) { started = false; continue; }
    if (!started) {
      ctx.moveTo(px(i), py(v));
      started = true;
    } else {
      ctx.lineTo(px(i), py(v));
    }
  }
  ctx.stroke();

  for (let i = n - 1; i >= 0; i--) {
    if (hist[i]) {
      ctx.fillStyle = ok;
      ctx.beginPath();
      ctx.arc(px(i), py(hist[i]), 2.5, 0, Math.PI * 2);
      ctx.fill();
      break;
    }
  }
}

/* ---------- 状态 ---------- */

function setPill(text, kind) {
  const p = $("pill");
  p.textContent = text;
  p.className = "pill" + (kind ? " " + kind : "");
}

function renderClients(list) {
  const ul = $("client-list");
  ul.innerHTML = "";
  if (!list || !list.length) {
    ul.innerHTML = '<li class="empty">暂无设备</li>';
    $("clients-hint").textContent = "";
    return;
  }
  $("clients-hint").textContent = list.length + " 台在线";
  list.sort((a, b) => b.rssi - a.rssi);
  list.forEach((c) => {
    const li = document.createElement("li");
    const left = document.createElement("span");
    left.textContent = c.mac;
    const right = document.createElement("span");
    right.className = "rssi";
    right.textContent = c.rssi + " dBm · " + c.phy;
    li.appendChild(left);
    li.appendChild(right);
    ul.appendChild(li);
  });
}

function showAuthBox() {
  const v = $("in-auth").value;          // 0=密码 1=企业版 2=开放
  $("psk-box").hidden = v !== "0";
  $("eap-box").hidden = v !== "1";
}

function showDnsBox() {
  $("dns-box").hidden = $("in-dns-mode").value !== "1";
}

async function refresh() {
  try {
    const d = await jfetch("/api/status");

    $("up-ssid").textContent = d.sta_ssid || "未配置";
    $("rssi").textContent = d.rssi ? d.rssi + " dBm" : "-- dBm";
    $("ip").textContent = (d.sta_ip && d.sta_ip !== "0.0.0.0") ? d.sta_ip : "--";
    $("clients").textContent = d.clients || 0;
    $("uptime").textContent = fmtDur(d.uptime_s);
    $("heap").textContent = fmtHeap(d.free_heap);
    $("apch").textContent = d.ap_channel || "--";
    $("fw").textContent = "fw " + (d.fw || "--");
    $("ch").textContent = d.channel || "--";
    $("nat").textContent = d.has_ip ? "已开" : "待上行";

    if (d.channel) {
      $("band").textContent = d.channel > 14 ? "5GHz" : "2.4GHz";
    } else {
      $("band").textContent = "--";
    }

    /* 上行人话状态 —— 这是“连上却上不了网”时最需要的诊断信息 */
    let state = "--", tone = "muted";
    if (d.remote_ready === false) {
      state = "C5 协处理器未就绪：检查 SPI 排线 / C5 供电 / C5 固件";
    } else if (d.has_ip) {
      state = "上行已联网，客户端可以正常上网";
    } else if (d.sta_linked) {
      state = "已连上 WiFi，正在等 IP（DHCP）…";
    } else if (d.sta_configured) {
      state = "未连上：" + reasonText(d.reason);
    } else {
      state = "还没配置上行 WiFi";
    }
    $("up-state").textContent = state;

    setBars(d.rssi || 0);
    lastHist = d.hist || [];
    drawSpark(lastHist);
    renderClients(d.client_list);

    if (d.remote_ready === false) {
      setPill("C5 未就绪", "err");
    } else if (d.has_ip) {
      setPill("已联网", "ok");
    } else if (d.sta_linked) {
      setPill("等待 IP", "warn");
    } else if (d.sta_configured) {
      setPill("连接失败", "err");
    } else {
      setPill("待配置", "warn");
    }

    /* 只在第一次填表单，避免覆盖用户正在输入的内容 */
    if (!cfgLoaded) {
      if (d.sta_ssid) $("in-ssid").value = d.sta_ssid;
      if (d.ap_ssid) $("in-ap-ssid").value = d.ap_ssid;
      $("in-band").value = String(d.band_mode || 0);
      if (d.ap_channel) $("in-apch").value = String(d.ap_channel);

      $("in-auth").value = String(d.eap_enable || 0);
      $("in-eap-method").value = String(d.eap_method || 0);
      $("in-eap-phase2").value = String(d.eap_phase2 || 0);
      $("in-eap-skip").value = d.eap_skip_cert === false ? "0" : "1";
      if (d.eap_username) $("in-eap-user").value = d.eap_username;
      if (d.eap_identity) $("in-eap-id").value = d.eap_identity;

      $("in-dns-mode").value = String(d.dns_mode || 0);
      if (d.dns1) $("in-dns1").value = d.dns1;
      if (d.dns2) $("in-dns2").value = d.dns2;

      showAuthBox();
      showDnsBox();
      cfgLoaded = true;
    }
  } catch (e) {
    setPill("设备无响应", "err");
    $("rssi").textContent = "-- dBm";
    $("up-state").textContent = "读不到设备状态，检查是否还连着 ESP32 的热点";
  }
}

/* ---------- 扫描（异步：接口立刻返回，这里轮询结果） ---------- */

function bandLabel(ch) {
  return ch > 14 ? "5G" : "2.4G";
}

function renderScan(list) {
  const ul = $("scan-list");
  ul.innerHTML = "";
  list.sort((a, b) => b.rssi - a.rssi);
  list.forEach((ap) => {
    const li = document.createElement("li");
    const name = document.createElement("span");
    name.className = "name";
    name.textContent = ap.ssid + (ap.secure ? "  🔒" : "");
    const meta = document.createElement("span");
    meta.className = "meta";
    const tag = document.createElement("span");
    tag.className = ap.ch > 14 ? "tag5" : "";
    tag.textContent = bandLabel(ap.ch);
    meta.appendChild(tag);
    meta.appendChild(document.createTextNode(" ch" + ap.ch + " · " + ap.rssi + "dBm" + (ap.eap ? " · 企业级" : "")));
    li.appendChild(name);
    li.appendChild(meta);
    li.onclick = () => {
      $("in-ssid").value = ap.ssid;
      const sel = $("in-band");
      if (ap.ch > 14) sel.value = "2"; else sel.value = "1";
      /* 企业级网络自动切到 802.1X 表单，省得用户自己找 */
      if (ap.eap) { $("in-auth").value = "1"; showAuthBox(); }
      $("in-pass").focus();
    };
    ul.appendChild(li);
  });
  scanCacheAt = Date.now();
}

async function scan(force) {
  const btn = $("btn-scan");
  const hint = $("scan-hint");

  /* 10 秒内重复点：直接用上次结果，0 延迟 */
  if (!force && scanCacheAt && Date.now() - scanCacheAt < 10000 && $("scan-list").children.length) {
    hint.textContent = "用 10 秒内上次的结果，点“重新扫描”可强制重扫";
    return;
  }

  btn.disabled = true;
  hint.textContent = "扫描中…";
  $("scan-list").innerHTML = "";

  try {
    const r = await jfetch("/api/scan?force=1");
    if (r.state === "notready") {
      hint.textContent = "C5 未就绪：检查 SPI 接线、C5 供电和固件";
      btn.disabled = false;
      return;
    }
  } catch (e) {
    hint.textContent = "请求失败，设备可能在忙";
    btn.disabled = false;
    return;
  }

  const t0 = Date.now();
  let tries = 0;
  const timer = setInterval(async () => {
    tries++;
    try {
      const d = await jfetch("/api/scan_result");
      if (d.state === "done") {
        clearInterval(timer);
        btn.disabled = false;
        const secs = ((Date.now() - t0) / 1000).toFixed(1);
        if (d.list && d.list.length) {
          renderScan(d.list);
          hint.textContent = "扫到 " + d.list.length + " 个网络，用时 " + secs + " 秒。点一个填进下面的输入框";
        } else {
          hint.textContent = "没扫到任何网络，检查 C5 的 SPI 连接";
        }
      } else if (d.state === "err") {
        clearInterval(timer);
        btn.disabled = false;
        hint.textContent = "扫描失败，稍后重试";
      }
    } catch (e) { /* 继续等 */ }

    if (tries > 25) {
      clearInterval(timer);
      btn.disabled = false;
      hint.textContent = "扫描超时，稍后重试";
    }
  }, 400);
}

/* ---------- 保存 / 重启 / 恢复出厂 ---------- */

function setMsg(t) { $("msg").textContent = t; }

async function save() {
  const authMode = Number($("in-auth").value);   // 0=密码 1=企业版 2=开放
  const auth = authMode === 1;
  const body = {
    sta_ssid: $("in-ssid").value.trim(),
    ap_ssid: $("in-ap-ssid").value.trim(),
    band_mode: Number($("in-band").value),
    ap_channel: Number($("in-apch").value),
    dns_mode: Number($("in-dns-mode").value),
    dns1: $("in-dns1").value.trim() || "223.5.5.5",
    dns2: $("in-dns2").value.trim() || "119.29.29.29",
    eap_enable: authMode,
  };

  if (auth) {
    body.eap_method = Number($("in-eap-method").value);
    body.eap_phase2 = Number($("in-eap-phase2").value);
    body.eap_skip_cert = Number($("in-eap-skip").value);
    body.eap_username = $("in-eap-user").value.trim();
    body.eap_identity = $("in-eap-id").value.trim();
    const ep = $("in-eap-pass").value;
    if (ep) body.eap_pass = ep;
    if (!body.eap_username) {
      setMsg("企业版认证必须填用户名（学号 / 工号）");
      return;
    }
  } else if (authMode === 2) {
    /* 开放网络：没有 WiFi 密码，连上后由浏览器带到认证页 */
    body.sta_pass = "";
  } else {
    const pass = $("in-pass").value;
    if (pass) body.sta_pass = pass;
  }

  const appass = $("in-ap-pass").value;
  if (appass) body.ap_pass = appass;

  if (!body.sta_ssid) {
    setMsg("上行 WiFi 名称不能为空");
    return;
  }
  if (appass && appass.length < 8) {
    setMsg("热点密码至少 8 位，否则会变成开放网络");
    return;
  }

  setMsg("保存中…");
  try {
    const d = await jfetch("/api/config", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body),
    });
    if (d.code === 0) {
      setMsg("已保存，设备重启中，约 15 秒后自动刷新");
      setTimeout(() => location.reload(), 15000);
    } else {
      setMsg("保存失败");
    }
  } catch (e) {
    setMsg("已提交（设备可能已在重启），15 秒后刷新");
    setTimeout(() => location.reload(), 15000);
  }
}

async function reboot() {
  setMsg("重启中…");
  try { await jfetch("/api/reboot", { method: "POST" }); } catch (e) { }
  setMsg("设备正在重启，约 10 秒");
  setTimeout(() => location.reload(), 12000);
}

async function factory() {
  if (!confirm("恢复出厂会清除上行 WiFi 和热点设置，确定吗？")) return;
  if (!confirm("再确认一次：清除后需要重新配网。")) return;
  setMsg("正在恢复出厂…");
  try { await jfetch("/api/factory", { method: "POST" }); } catch (e) { }
  setMsg("已恢复出厂，热点恢复为 ESP32-NAT / 12345678");
  setTimeout(() => location.reload(), 12000);
}

/* ---------- 启动 ---------- */

(function init() {
  const sel = $("in-apch");
  for (let c = 1; c <= 13; c++) {
    const o = document.createElement("option");
    o.value = String(c);
    o.textContent = c + (c === 1 || c === 6 || c === 11 ? "（推荐）" : "");
    sel.appendChild(o);
  }

  $("btn-scan").onclick = () => scan(true);
  $("in-auth").onchange = showAuthBox;
  $("in-dns-mode").onchange = showDnsBox;
  $("btn-save").onclick = save;
  $("btn-reboot").onclick = reboot;
  $("btn-factory").onclick = factory;

  refresh();
  setInterval(refresh, 2000);

  window.addEventListener("resize", () => drawSpark(lastHist));
})();
