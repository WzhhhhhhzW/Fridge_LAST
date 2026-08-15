"use strict";

const byId = (id) => document.getElementById(id);
const state = {
  csrf: "",
  sessionExpires: 0,
  snapshot: null,
  overview: null,
  inventory: [],
  inventorySummary: {},
  inventoryFilter: "all",
  inventorySearch: "",
  history: [],
  historyRange: "1h",
  editingRecord: null,
  eventSource: null,
  fallbackTimer: null,
  refreshTimers: [],
  controlsBusy: false,
  adminPage: "overview",
  clients: [],
  firmware: [],
  otaDevices: [],
  otaDeployments: [],
  lastAlertId: 0,
  activeAlertId: 0,
  recognitions: [],
};

class ApiError extends Error {
  constructor(message, status, payload) {
    super(message);
    this.status = status;
    this.payload = payload;
  }
}

function node(tag, className, text) {
  const element = document.createElement(tag);
  if (className) element.className = className;
  if (text !== undefined) element.textContent = String(text);
  return element;
}

function formatTime(timestamp, withDate = false) {
  if (!timestamp) return "--";
  const date = new Date(Number(timestamp) * 1000);
  return new Intl.DateTimeFormat("zh-CN", {
    month: withDate ? "2-digit" : undefined,
    day: withDate ? "2-digit" : undefined,
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
    hour12: false,
  }).format(date);
}

function formatDuration(seconds) {
  const value = Math.max(0, Number(seconds) || 0);
  if (value < 60) return `${Math.round(value)}秒`;
  if (value < 3600) return `${Math.floor(value / 60)}分`;
  if (value < 86400) return `${Math.floor(value / 3600)}时 ${Math.floor(value % 3600 / 60)}分`;
  return `${Math.floor(value / 86400)}天 ${Math.floor(value % 86400 / 3600)}时`;
}

function fanLabel(value) {
  return ["关闭", "低风 50%", "强风 100%"][Number(value)] || "未知";
}

function commandLabel(status) {
  return {
    queued: "等待下发",
    pending: "等待设备确认",
    confirmed: "设备已确认",
    sent: "一次性命令已下发",
    timeout: "确认超时",
    superseded: "已被新指令覆盖",
  }[status] || status || "未知";
}

function actionLabel(action) {
  return {
    admin_login: "管理员登录",
    admin_logout: "管理员退出",
    fridge_control: "冰箱控制",
    medicine_add: "新增药品",
    medicine_set: "编辑药品",
    medicine_delete: "清空槽位",
    quantity_set: "设置数量",
    quantity_restock: "补充库存",
    quantity_take: "取出药品",
    health_assess: "AI健康初筛",
  }[action] || action || "系统操作";
}

function toast(message, type = "success") {
  const item = node("div", `toast ${type}`, message);
  byId("toastRegion").append(item);
  window.setTimeout(() => item.remove(), 3800);
}

async function api(path, options = {}) {
  const method = String(options.method || "GET").toUpperCase();
  const headers = new Headers(options.headers || {});
  let body = options.body;
  if (body && typeof body !== "string") {
    headers.set("Content-Type", "application/json");
    body = JSON.stringify(body);
  }
  if (!["GET", "HEAD", "OPTIONS"].includes(method) && state.csrf && path !== "/api/admin/login") {
    headers.set("X-CSRF-Token", state.csrf);
  }
  const response = await fetch(path, {
    method,
    headers,
    body,
    credentials: "same-origin",
    cache: "no-store",
  });
  let payload = {};
  try { payload = await response.json(); } catch (_) { payload = {}; }
  if (!response.ok) {
    if (response.status === 401 && path !== "/api/admin/login") showLogin();
    throw new ApiError(payload.error || `请求失败 (${response.status})`, response.status, payload);
  }
  return payload;
}

function stopLiveUpdates() {
  if (state.eventSource) state.eventSource.close();
  state.eventSource = null;
  if (state.fallbackTimer) window.clearInterval(state.fallbackTimer);
  state.fallbackTimer = null;
  state.refreshTimers.forEach((timer) => window.clearInterval(timer));
  state.refreshTimers = [];
}

function showLogin(message = "") {
  stopLiveUpdates();
  state.csrf = "";
  byId("authLoading").classList.add("is-hidden");
  byId("adminApp").classList.add("is-hidden");
  byId("loginView").classList.remove("is-hidden");
  byId("loginError").textContent = message;
  byId("adminPassword").value = "";
}

function showApp() {
  byId("authLoading").classList.add("is-hidden");
  byId("loginView").classList.add("is-hidden");
  byId("adminApp").classList.remove("is-hidden");
  setAdminRoute();
}

async function checkSession() {
  try {
    const session = await api("/api/admin/session");
    state.csrf = session.csrf_token;
    state.sessionExpires = session.expires_at;
    showApp();
    await startDashboard();
  } catch (_) {
    showLogin();
  }
}

async function handleLogin(event) {
  event.preventDefault();
  const button = byId("loginButton");
  button.disabled = true;
  button.textContent = "正在验证…";
  byId("loginError").textContent = "";
  try {
    const result = await api("/api/admin/login", {
      method: "POST",
      body: { password: byId("adminPassword").value },
    });
    state.csrf = result.csrf_token;
    state.sessionExpires = result.expires_at;
    showApp();
    toast("管理员登录成功");
    await startDashboard();
  } catch (error) {
    const retry = error.payload && error.payload.retry_after;
    byId("loginError").textContent = retry ? `${error.message}（约${Math.ceil(retry / 60)}分钟）` : error.message;
  } finally {
    button.disabled = false;
    button.textContent = "安全登录";
  }
}

async function handleLogout() {
  try { await api("/api/admin/logout", { method: "POST" }); } catch (_) { /* session is cleared locally too */ }
  showLogin("已安全退出管理控制台");
}

async function startDashboard() {
  stopLiveUpdates();
  await Promise.allSettled([
    refreshBootstrap(), refreshOverview(), refreshInventory(), refreshHistory(), refreshHealth(), refreshAudit(),
    refreshClients(), refreshLogs(), refreshPowerAlerts(), refreshFirmwareAdmin(), refreshOtaAdmin(), refreshRecognitions(),
  ]);
  connectStream();
  state.refreshTimers.push(window.setInterval(refreshOverview, 5000));
  state.refreshTimers.push(window.setInterval(refreshInventory, 10000));
  state.refreshTimers.push(window.setInterval(refreshHistory, 15000));
  state.refreshTimers.push(window.setInterval(refreshAudit, 5000));
  state.refreshTimers.push(window.setInterval(refreshClients, 10000));
  state.refreshTimers.push(window.setInterval(refreshLogs, 10000));
  state.refreshTimers.push(window.setInterval(refreshPowerAlerts, 5000));
  state.refreshTimers.push(window.setInterval(refreshRecognitions, 5000));
  state.refreshTimers.push(window.setInterval(refreshFirmwareAdmin, 30000));
  state.refreshTimers.push(window.setInterval(refreshOtaAdmin, 2000));
}

async function refreshBootstrap() {
  try {
    const data = await api("/api/admin/bootstrap");
    renderSnapshot(data);
  } catch (error) { toast(error.message, "error"); }
}

function connectStream() {
  if (state.eventSource) state.eventSource.close();
  const source = new EventSource("/api/admin/stream", { withCredentials: true });
  state.eventSource = source;
  source.addEventListener("snapshot", (event) => {
    try {
      renderSnapshot(JSON.parse(event.data));
      if (state.fallbackTimer) window.clearInterval(state.fallbackTimer);
      state.fallbackTimer = null;
    } catch (_) { /* ignore malformed event */ }
  });
  source.addEventListener("session", () => showLogin("管理会话已过期，请重新登录"));
  source.onerror = () => {
    if (!state.fallbackTimer) state.fallbackTimer = window.setInterval(refreshBootstrap, 2000);
  };
}

function renderSnapshot(data) {
  state.snapshot = data;
  const device = data.status || {};
  const target = data.control || {};
  const online = Boolean(device.online);
  byId("onlinePill").classList.toggle("offline", !online);
  byId("onlinePill").lastChild.textContent = online ? "设备在线" : "设备离线";
  byId("metricOnline").textContent = online ? "在线" : "离线";
  byId("metricHeartbeat").textContent = online ? `心跳 ${Number(device.last_seen_age_seconds || 0).toFixed(1)}秒前` : "等待W800心跳";
  const clients = data.clients || {};
  byId("metricClients").textContent = `${clients.online || 0} 在线`;
  byId("metricClientsTotal").textContent = `已登记 ${clients.registered || 0} 台`;
  byId("metricTemp").textContent = `${Number(device.current_temp || 0).toFixed(1)}℃`;
  byId("metricSetTemp").textContent = `目标 ${Number(target.set_temp ?? device.set_temp ?? 0)}℃`;
  byId("actualTemperature").textContent = `${Number(device.current_temp || 0).toFixed(1)}℃`;
  byId("targetTemperature").textContent = `${Number(target.set_temp ?? 0)}℃`;
  byId("twinTemp").textContent = `${Number(device.current_temp || 0).toFixed(1)}°`;
  byId("fridgeTwin").classList.toggle("offline", !online);
  byId("twinDoor").classList.toggle("open", Boolean(device.door));
  byId("twinFan").textContent = fanLabel(device.fan);
  byId("twinCool").textContent = device.cool ? "运行中" : "关闭";
  byId("twinWater").textContent = device.water_cool ? "运行中" : "关闭";
  byId("twinVision").textContent = device.vision_name || "暂无";
  byId("coolState").textContent = `设备状态：${device.cool ? "开启" : "关闭"}`;
  byId("waterState").textContent = `设备状态：${device.water_cool ? "开启" : "关闭"}`;

  const targetTemp = Number(target.set_temp ?? 0);
  byId("tempRange").value = String(targetTemp);
  byId("tempInput").value = String(targetTemp);
  byId("coolSwitch").checked = Boolean(target.cool);
  byId("waterSwitch").checked = Boolean(target.water_cool);
  document.querySelectorAll("#fanControl button").forEach((button) => {
    button.classList.toggle("active", Number(button.dataset.fan) === Number(target.fan));
  });
  setControlsEnabled(online && !state.controlsBusy);

  const pending = Boolean(data.control_pending);
  const tag = byId("controlPending");
  tag.className = `tag ${pending ? "pending" : "confirmed"}`;
  tag.textContent = pending ? "等待设备确认" : "状态已同步";
  renderCommand(data.command);
  const powerSettings = data.power_settings || {};
  if (powerSettings.absolute_limit_w !== undefined) byId("powerLimitInput").value = String(powerSettings.absolute_limit_w);
  if (powerSettings.relative_multiplier !== undefined) byId("powerMultiplierInput").value = String(powerSettings.relative_multiplier);
  if (powerSettings.relative_step_w !== undefined) byId("powerStepInput").value = String(powerSettings.relative_step_w);
  maybeShowPowerAlert(data.power_alerts || []);
  if (Array.isArray(data.recognitions)) {
    state.recognitions = data.recognitions;
    renderRecognitions(state.recognitions);
  }
  byId("footerUpdate").textContent = `最近同步 ${new Date().toLocaleTimeString("zh-CN", { hour12: false })}`;
}

function renderRecognitions(records) {
  const list = byId("recognitionList");
  list.replaceChildren();
  (records || []).slice(0, 5).forEach((record, index) => {
    const item = node("div", "recognition-item");
    const rank = node("span", "recognition-rank", String(index + 1).padStart(2, "0"));
    const content = node("div", "recognition-content");
    const code = Number(record.code);
    content.append(
      node("strong", "", record.name || "未知药品"),
      node("small", "", `视觉类别 0x${Number.isFinite(code) ? code.toString(16).toUpperCase().padStart(2, "0") : "--"}`),
    );
    item.append(rank, content, node("time", "", formatTime(record.last_seen, true)));
    list.append(item);
  });
  if (!list.children.length) list.append(node("div", "recognition-empty", "暂无药品识别记录"));
}

async function refreshRecognitions() {
  try {
    const data = await api("/api/admin/recognitions?limit=5");
    state.recognitions = data.records || [];
    renderRecognitions(state.recognitions);
  } catch (_) { /* SSE or the next polling cycle will retry */ }
}

function setControlsEnabled(enabled) {
  document.querySelectorAll(".control-panel button, .control-panel input").forEach((element) => { element.disabled = !enabled; });
  byId("controlHint").textContent = enabled ? "点击控件后立即下发，并等待RA6M5下一次状态确认。" : "设备离线或请求处理中，控制功能已锁定。";
}

function renderCommand(command) {
  const container = byId("commandState");
  if (!command) {
    container.className = "command-state empty";
    container.querySelector("strong").textContent = "暂无控制指令";
    container.querySelector("p").textContent = "发送控制后将在这里显示设备确认状态";
    return;
  }
  container.className = `command-state ${command.status || "pending"}`;
  const fieldText = Object.entries(command.fields || {}).map(([key, value]) => `${key}=${value}`).join(" · ");
  container.querySelector("strong").textContent = `${commandLabel(command.status)} · #${command.id}`;
  container.querySelector("p").textContent = `${fieldText || "控制指令"} · ${formatTime(command.requested_at)}`;
}

async function sendControl(fields) {
  if (!state.snapshot || !state.snapshot.status || !state.snapshot.status.online) {
    toast("设备离线，不能发送控制指令", "error");
    return;
  }
  state.controlsBusy = true;
  setControlsEnabled(false);
  try {
    const result = await api("/api/fridge/control", { method: "POST", body: fields });
    renderSnapshot(result);
    toast(`控制指令已提交 #${result.command_id}`);
    refreshAudit();
  } catch (error) {
    toast(error.message, "error");
    await refreshBootstrap();
  } finally {
    state.controlsBusy = false;
    if (state.snapshot) setControlsEnabled(Boolean(state.snapshot.status.online));
  }
}

function targetTemperature() {
  return Number(state.snapshot && state.snapshot.control ? state.snapshot.control.set_temp : byId("tempInput").value) || 0;
}

async function refreshOverview() {
  try {
    const data = await api("/api/public/dashboard/overview");
    state.overview = data;
    const inventory = data.inventory || {};
    const metrics = data.metrics || {};
    const server = data.server || {};
    const clients = data.clients || {};
    byId("metricClients").textContent = `${clients.online || 0} 在线`;
    byId("metricClientsTotal").textContent = `已登记 ${clients.registered || 0} 台`;
    byId("metricQuantity").textContent = `${inventory.total_quantity || 0} 件`;
    byId("metricSlots").textContent = `占用 ${inventory.occupied || 0} / ${inventory.capacity || 32}`;
    byId("metricAi").textContent = `${metrics.today_ai_requests || 0} 次`;
    byId("metricSuccess").textContent = `请求成功率 ${Number(metrics.success_rate || 0).toFixed(1)}%`;
    byId("metricUptime").textContent = formatDuration(server.uptime_seconds);
    byId("metricLatency").textContent = `平均延迟 ${Number(metrics.average_latency_ms || 0).toFixed(0)}ms`;
    byId("cpuValue").textContent = `${Number(server.cpu_percent || 0).toFixed(0)}%`;
    byId("memoryValue").textContent = `${Number(server.memory_percent || 0).toFixed(0)}%`;
    byId("cpuGauge").style.setProperty("--value", Math.min(100, Number(server.cpu_percent || 0)));
    byId("memoryGauge").style.setProperty("--value", Math.min(100, Number(server.memory_percent || 0)));
    byId("serverModel").textContent = server.model || "--";
    byId("processMemory").textContent = `${Number(server.process_memory_mb || 0).toFixed(1)} MB`;
    byId("serverPorts").textContent = `W800 ${server.tcp_port || "--"} / HTTP ${server.http_port || "--"}`;
    renderAlerts(data.alerts || []);
    maybeShowPowerAlert(data.power_alerts || []);
  } catch (_) { /* keep the last overview during a short network interruption */ }
}

function renderAlerts(alerts) {
  const list = byId("alertList");
  list.replaceChildren();
  if (!alerts.length) {
    list.append(node("div", "alert-item ok", "运行状态正常，暂无活动告警"));
    return;
  }
  alerts.slice(0, 3).forEach((alert) => {
    list.append(node("div", `alert-item ${alert.level || ""}`, `${alert.title} · ${alert.detail}`));
  });
}

async function refreshInventory() {
  try {
    const data = await api("/api/public/dashboard/inventory");
    state.inventory = data.records || [];
    state.inventorySummary = data.summary || {};
    byId("inventoryTotal").textContent = String(state.inventorySummary.total_quantity || 0);
    byId("inventoryOccupied").textContent = String(state.inventorySummary.occupied || 0);
    byId("inventoryExpiring").textContent = String(state.inventorySummary.expiring || 0);
    byId("inventoryExpired").textContent = String(state.inventorySummary.expired || 0);
    renderInventory();
  } catch (error) { toast(`库存读取失败：${error.message}`, "error"); }
}

function renderInventory() {
  const grid = byId("inventoryGrid");
  grid.replaceChildren();
  const query = state.inventorySearch.trim().toLowerCase();
  const records = state.inventory.filter((record) => {
    const status = record.valid ? record.expiry_status : "empty";
    const filterMatch = state.inventoryFilter === "all" || state.inventoryFilter === status;
    const searchMatch = !query || String(record.name || "").toLowerCase().includes(query) || String(record.efficacy || "").toLowerCase().includes(query);
    return filterMatch && searchMatch;
  });
  if (!records.length) {
    const empty = node("p", "control-hint", "没有符合当前条件的槽位");
    grid.append(empty);
    return;
  }
  records.forEach((record) => {
    const status = record.valid ? record.expiry_status : "empty";
    const button = node("button", `slot-card ${status}`);
    button.type = "button";
    const head = node("span", "slot-number");
    head.append(node("span", "", `SLOT ${String(Number(record.slot) + 1).padStart(2, "0")}`), node("i", "slot-status"));
    const title = node("strong", "", record.valid ? record.name : "＋ 录入药品");
    const detail = node("small", "", record.valid ? `× ${record.quantity} · ${expiryText(record)}` : "当前为空槽");
    button.append(head, title, detail);
    button.addEventListener("click", () => openMedicine(record));
    grid.append(button);
  });
}

function expiryText(record) {
  if (record.expiry_status === "expired") return `已过期 ${Math.abs(record.days_remaining || 0)}天`;
  if (record.expiry_status === "expiring") return `剩余 ${record.days_remaining}天`;
  if (record.expiry_status === "normal") return `剩余 ${record.days_remaining}天`;
  return "日期未知";
}

function openMedicine(record) {
  state.editingRecord = record;
  byId("medicineSlot").value = String(record.slot);
  byId("medicineDialogTitle").textContent = `槽位 ${Number(record.slot) + 1} · ${record.valid ? "编辑药品" : "录入药品"}`;
  byId("medicineName").value = record.valid ? record.name : "";
  byId("medicineDate").value = record.valid ? record.production_date : "";
  byId("medicineShelf").value = record.valid ? record.shelf_life : "";
  byId("medicineQuantity").value = String(record.valid ? record.quantity : 1);
  byId("medicineEfficacy").value = record.valid ? record.efficacy : "";
  byId("medicineError").textContent = "";
  byId("quantityActions").classList.toggle("is-hidden", !record.valid);
  byId("deleteMedicine").classList.toggle("is-hidden", !record.valid);
  if (!byId("medicineDialog").open) byId("medicineDialog").showModal();
}

async function saveMedicine(event) {
  event.preventDefault();
  const slot = Number(byId("medicineSlot").value);
  const payload = {
    name: byId("medicineName").value.trim(),
    production_date: byId("medicineDate").value.trim(),
    shelf_life: byId("medicineShelf").value.trim(),
    efficacy: byId("medicineEfficacy").value.trim(),
    quantity: Number(byId("medicineQuantity").value),
  };
  try {
    await api(`/api/medicines/${slot}`, { method: "PUT", body: payload });
    byId("medicineDialog").close();
    toast(`槽位 ${slot + 1} 已保存`);
    await Promise.all([refreshInventory(), refreshAudit(), refreshOverview()]);
  } catch (error) { byId("medicineError").textContent = error.message; }
}

async function quantityOperation(operation, value) {
  const record = state.editingRecord;
  if (!record || !record.valid) return;
  if (operation === "take" && Number(value) >= Number(record.quantity)) {
    const accepted = await confirmAction("确认取空该槽位？", "取药后数量将变为0，服务器会自动清空这个槽位的药品资料。");
    if (!accepted) return;
  }
  try {
    const result = await api(`/api/medicines/${record.slot}/quantity`, {
      method: "PATCH",
      body: { operation, value: Number(value) },
    });
    toast(`库存数量已更新为 ${result.quantity}`);
    await Promise.all([refreshInventory(), refreshAudit(), refreshOverview()]);
    const current = state.inventory.find((item) => item.slot === record.slot);
    if (!current || !current.valid) byId("medicineDialog").close();
    else openMedicine(current);
  } catch (error) { byId("medicineError").textContent = error.message; }
}

async function deleteMedicine() {
  const record = state.editingRecord;
  if (!record || !record.valid) return;
  const accepted = await confirmAction("确认清空槽位？", `将删除槽位 ${record.slot + 1} 的“${record.name}”及其全部库存信息，此操作不可撤销。`);
  if (!accepted) return;
  try {
    await api(`/api/medicines/${record.slot}`, { method: "DELETE" });
    byId("medicineDialog").close();
    toast(`槽位 ${record.slot + 1} 已清空`);
    await Promise.all([refreshInventory(), refreshAudit(), refreshOverview()]);
  } catch (error) { byId("medicineError").textContent = error.message; }
}

function confirmAction(title, message) {
  const dialog = byId("confirmDialog");
  byId("confirmTitle").textContent = title;
  byId("confirmMessage").textContent = message;
  dialog.showModal();
  return new Promise((resolve) => {
    dialog.addEventListener("close", () => resolve(dialog.returnValue === "confirm"), { once: true });
  });
}

async function refreshHistory() {
  try {
    const data = await api(`/api/public/dashboard/history?range=${encodeURIComponent(state.historyRange)}`);
    state.history = data.points || [];
    drawChart();
    drawAdminMetricCharts();
  } catch (_) { /* retain last chart */ }
}

function drawChart() {
  const canvas = byId("temperatureChart");
  const empty = byId("chartEmpty");
  const rect = canvas.getBoundingClientRect();
  const ratio = Math.min(2, window.devicePixelRatio || 1);
  canvas.width = Math.max(1, Math.round(rect.width * ratio));
  canvas.height = Math.max(1, Math.round(rect.height * ratio));
  const context = canvas.getContext("2d");
  context.setTransform(ratio, 0, 0, ratio, 0, 0);
  context.clearRect(0, 0, rect.width, rect.height);
  const points = state.history;
  empty.classList.toggle("is-hidden", points.length > 1);
  if (points.length < 2) return;
  const padding = { left: 42, right: 16, top: 12, bottom: 28 };
  const width = rect.width - padding.left - padding.right;
  const height = rect.height - padding.top - padding.bottom;
  const values = points.flatMap((point) => point.online
    ? [Number(point.current_temp), Number(point.set_temp)]
    : [0, 0]).filter(Number.isFinite);
  let min = Math.floor(Math.min(...values) - 1);
  let max = Math.ceil(Math.max(...values) + 1);
  if (max - min < 4) { min -= 2; max += 2; }
  context.font = "9px Segoe UI";
  context.fillStyle = "#8192a5";
  context.strokeStyle = "rgba(92,126,166,.14)";
  context.lineWidth = 1;
  for (let line = 0; line <= 4; line += 1) {
    const y = padding.top + height * line / 4;
    context.beginPath(); context.moveTo(padding.left, y); context.lineTo(rect.width - padding.right, y); context.stroke();
    context.fillText(`${(max - (max - min) * line / 4).toFixed(1)}°`, 3, y + 3);
  }
  const xFor = (index) => padding.left + width * index / (points.length - 1);
  const yFor = (value) => padding.top + height * (max - value) / (max - min);
  const drawOnlineLine = (key, color, dash = []) => {
    context.beginPath();
    context.strokeStyle = color; context.lineWidth = 2; context.setLineDash(dash);
    let open = false;
    points.forEach((point, index) => {
      const value = Number(point[key]);
      if (!point.online || !Number.isFinite(value)) { open = false; return; }
      const x = xFor(index); const y = yFor(value);
      if (!open) { context.moveTo(x, y); open = true; } else context.lineTo(x, y);
    });
    context.stroke(); context.setLineDash([]);
  };
  drawOnlineLine("current_temp", "#2e8ef5");
  drawOnlineLine("set_temp", "#8b76e7", [5, 4]);
  context.beginPath();
  context.strokeStyle = "#9aa9ba"; context.lineWidth = 2.4;
  let offlineOpen = false;
  points.forEach((point, index) => {
    if (point.online) { offlineOpen = false; return; }
    const x = xFor(index); const y = yFor(0);
    if (!offlineOpen) { context.moveTo(x, y); offlineOpen = true; } else context.lineTo(x, y);
  });
  context.stroke();
  [0, Math.floor((points.length - 1) / 2), points.length - 1].forEach((index) => {
    const text = formatTime(points[index].ts);
    const x = xFor(index) - context.measureText(text).width / 2;
    context.fillText(text, Math.max(padding.left - 4, Math.min(x, rect.width - padding.right - 42)), rect.height - 6);
  });
}

async function submitHealth(event) {
  event.preventDefault();
  const symptom = byId("symptomInput").value.trim();
  if (!symptom) return;
  const button = byId("healthSubmit");
  const started = performance.now();
  button.disabled = true; button.textContent = "AI分析中…";
  byId("diagnosisResult").textContent = "正在分析";
  byId("medicineResult").textContent = "正在校验库存";
  try {
    const result = await api("/api/health/recommend", { method: "POST", body: { symptom } });
    byId("diagnosisResult").textContent = result.diagnosis || "暂无诊断结果";
    byId("medicineResult").textContent = result.medicine || "无对应药品";
    byId("aiLatency").textContent = `${Math.round(performance.now() - started)} ms`;
    byId("aiResult").classList.remove("empty");
    toast("AI初筛完成");
    await Promise.all([refreshHealth(), refreshAudit(), refreshOverview()]);
  } catch (error) {
    byId("diagnosisResult").textContent = "诊断失败";
    byId("medicineResult").textContent = "暂无用药建议";
    byId("aiLatency").textContent = `${Math.round(performance.now() - started)} ms`;
    toast(error.message, "error");
  } finally { button.disabled = false; button.textContent = "启动智能初筛"; }
}

async function refreshHealth() {
  try {
    const data = await api("/api/public/dashboard/health?limit=6");
    const list = byId("healthHistory");
    list.replaceChildren();
    (data.records || []).forEach((record) => {
      const item = node("div", "health-item");
      const content = node("div");
      content.append(node("strong", "", `${record.symptom_summary} → ${record.diagnosis}`), node("small", "", `推荐：${record.medicine} · ${Number(record.latency_ms || 0).toFixed(0)}ms`));
      item.append(content, node("time", "", formatTime(record.ts, true)));
      list.append(item);
    });
  } catch (_) { /* retain last records */ }
}

async function refreshAudit() {
  try {
    const data = await api("/api/admin/audit?limit=80");
    const timeline = byId("auditTimeline");
    timeline.replaceChildren();
    const records = data.records || [];
    if (!records.length) { timeline.append(node("p", "control-hint", "暂无管理操作记录")); return; }
    records.forEach((record) => {
      const item = node("div", "audit-item");
      const dot = node("span", "audit-dot");
      if (record.category === "inventory") dot.style.setProperty("--audit", "#7969e8");
      if (record.category === "ai") dot.style.setProperty("--audit", "#13aebf");
      if (record.result === "failed" || record.result === "denied" || record.result === "timeout") dot.style.setProperty("--audit", "#df5d6f");
      const content = node("div", "audit-content");
      const header = node("div");
      header.append(node("strong", "", actionLabel(record.action)), node("time", "", formatTime(record.ts, true)));
      const detail = node("p", "", [record.target, record.summary].filter(Boolean).join(" · ") || record.actor);
      const result = node("span", `audit-result ${record.result}`, commandLabel(record.result));
      content.append(header, detail, result);
      item.append(dot, content);
      timeline.append(item);
    });
  } catch (_) { /* session handler deals with unauthorized */ }
}

const ADMIN_ROUTE_TITLES = {
  overview: "运营总览",
  devices: "手机设备",
  monitor: "运行监控",
  inventory: "智慧仓储",
  logs: "日志告警",
  firmware: "固件中心",
  ai: "AI助手",
};

function setAdminRoute(options = {}) {
  const segment = location.pathname.split("/").filter(Boolean)[1] || "overview";
  state.adminPage = Object.prototype.hasOwnProperty.call(ADMIN_ROUTE_TITLES, segment) ? segment : "overview";
  document.querySelectorAll("[data-admin-page]").forEach((section) => { section.hidden = section.dataset.adminPage !== state.adminPage; });
  document.querySelectorAll("[data-admin-route]").forEach((link) => link.classList.toggle("active", link.dataset.adminRoute === state.adminPage));
  document.title = `${ADMIN_ROUTE_TITLES[state.adminPage]} · 智慧药品冷藏系统`;
  if (options.scrollToTop) window.scrollTo({ top: 0, left: 0, behavior: "auto" });
  if (state.adminPage === "monitor" && !byId("adminApp").classList.contains("is-hidden")) {
    window.requestAnimationFrame(() => { drawChart(); drawAdminMetricCharts(); });
  }
}

function handleAdminNavigation(event) {
  if (event.defaultPrevented || event.button !== 0 || event.metaKey || event.ctrlKey || event.shiftKey || event.altKey) return;
  const target = event.target instanceof Element ? event.target.closest("a[data-admin-route]") : null;
  if (!target) return;
  const url = new URL(target.href, location.href);
  if (url.origin !== location.origin) return;
  event.preventDefault();
  if (url.pathname !== location.pathname) history.pushState({}, "", url.pathname);
  setAdminRoute({ scrollToTop: true });
}

function maybeShowPowerAlert(alerts) {
  const alert = alerts.find((item) => !item.acknowledged);
  if (!alert || Number(alert.id) <= state.lastAlertId) return;
  state.lastAlertId = Number(alert.id);
  state.activeAlertId = Number(alert.id);
  byId("powerAlertMessage").textContent = alert.detail || `当前总功率 ${Number(alert.power_w || 0).toFixed(1)}W，请检查制冷系统。`;
  if (!byId("powerAlertDialog").open) byId("powerAlertDialog").showModal();
}

async function refreshClients() {
  try {
    const data = await api("/api/admin/clients");
    state.clients = data.clients || [];
    const summary = data.summary || {};
    byId("clientOnlineCount").textContent = String(summary.online || 0);
    byId("clientRegisteredCount").textContent = String(summary.registered || 0);
    byId("clientDisabledCount").textContent = String(summary.disabled || 0);
    byId("metricClients").textContent = `${summary.online || 0} 在线`;
    byId("metricClientsTotal").textContent = `已登记 ${summary.registered || 0} 台`;
    renderClients();
  } catch (_) { /* keep the last client list */ }
}

function renderClients() {
  const grid = byId("clientGrid");
  grid.replaceChildren();
  state.clients.forEach((client) => {
    const card = node("article", `client-card ${client.enabled ? "" : "disabled"}`);
    const head = node("div", "client-head");
    head.append(node("h3", "", client.name), node("span", `client-state ${client.enabled ? (client.online ? "" : "offline") : "disabled"}`, client.enabled ? (client.online ? "在线" : "离线") : "已停用"));
    const details = node("dl");
    [["Token", client.token_prefix], ["最近请求", client.last_seen ? formatTime(client.last_seen, true) : "从未使用"], ["创建时间", formatTime(client.created_at, true)], ["备注", client.notes || "--"]].forEach(([key, value]) => { const row = node("div"); row.append(node("dt", "", key), node("dd", "", value)); details.append(row); });
    const actions = node("div", "client-actions");
    const edit = node("button", "", "编辑名称"); edit.type = "button"; edit.addEventListener("click", () => editClient(client));
    const toggle = node("button", client.enabled ? "danger" : "", client.enabled ? "停用" : "重新启用"); toggle.type = "button"; toggle.addEventListener("click", () => toggleClient(client));
    actions.append(edit, toggle);
    if (!client.is_legacy) { const reset = node("button", "", "重置Token"); reset.type = "button"; reset.addEventListener("click", () => regenerateClientToken(client)); actions.append(reset); }
    card.append(head, details, actions); grid.append(card);
  });
  if (!grid.children.length) grid.append(node("p", "control-hint", "尚未创建手机客户端Token"));
}

function showGeneratedToken(token) {
  byId("generatedToken").textContent = token;
  if (!byId("tokenDialog").open) byId("tokenDialog").showModal();
}

async function createClient(event) {
  event.preventDefault();
  try {
    const result = await api("/api/admin/clients", { method: "POST", body: { name: byId("clientName").value.trim(), notes: byId("clientNotes").value.trim() } });
    event.target.reset(); showGeneratedToken(result.token); toast("客户端已创建"); await Promise.all([refreshClients(), refreshAudit()]);
  } catch (error) { toast(error.message, "error"); }
}

async function editClient(client) {
  const name = window.prompt("输入新的客户端名称", client.name);
  if (name === null || !name.trim()) return;
  try { await api(`/api/admin/clients/${client.id}`, { method: "PATCH", body: { name: name.trim() } }); toast("客户端名称已更新"); await refreshClients(); } catch (error) { toast(error.message, "error"); }
}

async function toggleClient(client) {
  const accepted = await confirmAction(client.enabled ? "确认停用该手机？" : "确认重新启用？", client.is_legacy && client.enabled ? "这是旧版共享Token，停用会阻止所有仍使用旧Token的手机。" : `客户端“${client.name}”的后续请求将${client.enabled ? "被服务器拒绝" : "重新获准访问"}。`);
  if (!accepted) return;
  try { await api(`/api/admin/clients/${client.id}`, { method: "PATCH", body: { enabled: !client.enabled } }); toast(client.enabled ? "客户端已停用" : "客户端已启用"); await Promise.all([refreshClients(), refreshAudit()]); } catch (error) { toast(error.message, "error"); }
}

async function regenerateClientToken(client) {
  const accepted = await confirmAction("确认重置Token？", "原Token会立即失效，必须把新Token重新填写到对应手机。此操作不可撤销。");
  if (!accepted) return;
  try { const result = await api(`/api/admin/clients/${client.id}/regenerate-token`, { method: "POST" }); showGeneratedToken(result.token); await Promise.all([refreshClients(), refreshAudit()]); } catch (error) { toast(error.message, "error"); }
}

async function refreshLogs() {
  try {
    const params = new URLSearchParams({ limit: "300" });
    if (byId("logLevel").value) params.set("level", byId("logLevel").value);
    if (byId("logQuery").value.trim()) params.set("q", byId("logQuery").value.trim());
    const data = await api(`/api/admin/logs?${params}`), list = byId("serverLogList"); list.replaceChildren();
    (data.records || []).forEach((record) => { const row = node("div", "log-row"); row.append(node("time", "", formatTime(record.ts)), node("b", record.level, record.level), node("span", "", record.module), node("p", "", record.message)); list.append(row); });
    if (!list.children.length) list.append(node("p", "control-hint", "暂无符合条件的服务器日志"));
  } catch (_) { /* keep last logs */ }
}

async function savePowerSettings(event) {
  event.preventDefault();
  try {
    const result = await api("/api/admin/power-settings", { method: "PATCH", body: { absolute_limit_w: Number(byId("powerLimitInput").value), relative_multiplier: Number(byId("powerMultiplierInput").value), relative_step_w: Number(byId("powerStepInput").value) } });
    toast(`功率上限已设为 ${result.settings.absolute_limit_w}W`); await refreshAudit();
  } catch (error) { toast(error.message, "error"); }
}

async function refreshPowerAlerts() {
  try {
    const activeOnly = byId("powerAlertFilter").value === "active";
    const data = await api(`/api/admin/alerts?limit=50${activeOnly ? "&active=1" : ""}`), list = byId("powerAlertList"); list.replaceChildren();
    (data.alerts || []).slice(0, 20).forEach((alert) => { const row = node("div", `alert-item ${alert.status === "active" ? "danger" : "ok"}`); const text = node("div"); text.append(node("strong", "", alert.status === "active" ? "活动功率异常" : "已恢复"), node("small", "", `${alert.detail} · ${formatTime(alert.started_at, true)}`)); row.append(text); if (!alert.acknowledged) { const ack = node("button", "mini-button", "确认"); ack.type = "button"; ack.addEventListener("click", () => acknowledgePowerAlert(alert.id)); row.append(ack); } list.append(row); });
    if (!list.children.length) list.append(node("p", "control-hint", "暂无功率异常记录"));
    maybeShowPowerAlert((data.alerts || []).filter((alert) => alert.status === "active"));
  } catch (_) { /* keep last alerts */ }
}

async function acknowledgePowerAlert(id) {
  try { await api(`/api/admin/alerts/${id}/ack`, { method: "POST" }); if (byId("powerAlertDialog").open) byId("powerAlertDialog").close(); state.activeAlertId = 0; toast("功率告警已确认"); await refreshPowerAlerts(); } catch (error) { toast(error.message, "error"); }
}

function firmwareComponentLabel(value) { return { main: "RA6M5主控", hmi: "TJC串口屏", vision: "视觉模型" }[value] || value; }
function fileSize(value) { const bytes = Number(value) || 0; return bytes >= 1048576 ? `${(bytes / 1048576).toFixed(1)}MB` : `${(bytes / 1024).toFixed(1)}KB`; }
async function refreshFirmwareAdmin() { try { const data = await api("/api/admin/firmware/releases"); state.firmware = data.releases || []; renderFirmwareAdmin(); } catch (_) { /* keep last versions */ } }
function otaStateLabel(value) {
  return {
    idle: "空闲", offered: "等待设备接受", erasing: "正在擦除", receiving: "正在接收",
    writing: "正在写入", transferring: "正在传输", verifying: "正在校验",
    waiting_reboot: "等待复位", rebooting: "正在复位", trial: "新固件试运行",
    queued: "已排队", succeeded: "升级成功", failed: "升级失败",
    rolled_back: "已回退", cancelled: "已取消",
  }[value] || value || "未知";
}
function otaDeployableArtifacts(device) {
  const result = [];
  state.firmware.forEach((release) => {
    if (release.component !== "main" || !release.published) return;
    (release.artifacts || []).forEach((artifact) => {
      if (artifact.kind === "ota" && artifact.image_valid && artifact.target_part === device.part_number) {
        result.push({ ...artifact, release });
      }
    });
  });
  return result;
}
async function refreshOtaAdmin() {
  try {
    const [devices, deployments] = await Promise.all([
      api("/api/admin/firmware/devices"),
      api("/api/admin/firmware/deployments?limit=30"),
    ]);
    state.otaDevices = devices.devices || [];
    state.otaDeployments = deployments.deployments || [];
    renderOtaAdmin();
    byId("otaRefreshState").textContent = `更新于 ${formatTime(Date.now() / 1000)}`;
  } catch (_) { /* keep last OTA snapshot */ }
}
function renderOtaAdmin() {
  const deviceGrid = byId("otaDeviceGrid"), deploymentGrid = byId("otaDeploymentGrid");
  deviceGrid.replaceChildren();
  state.otaDevices.forEach((device) => {
    const card = node("article", "ota-device-card"), head = node("div", "ota-card-head"), title = node("div");
    title.append(node("b", "", device.device_id), node("small", "", device.part_number));
    const badgeClass = device.online ? (device.ota_state === "idle" ? "" : "busy") : "offline";
    head.append(title, node("span", `ota-badge ${badgeClass}`, device.online ? otaStateLabel(device.ota_state) : "离线"));
    card.append(head);
    const meta = node("div", "ota-meta");
    [["应用版本", device.app_version], ["Bootloader", device.boot_version], ["活动Bank", `Bank ${device.active_bank}`], ["签名", "SCE9 / ECDSA-P256"]].forEach(([label, value]) => {
      const row = node("div"); row.append(node("span", "", label), node("b", "", value || "--")); meta.append(row);
    });
    card.append(meta);
    const actions = node("div", "ota-device-actions"), select = document.createElement("select");
    const artifacts = otaDeployableArtifacts(device);
    artifacts.forEach((artifact) => {
      const option = document.createElement("option");
      option.value = String(artifact.id);
      option.textContent = `${artifact.image_version} · ${fileSize(artifact.size_bytes)} · 已验签`;
      select.append(option);
    });
    if (!artifacts.length) {
      const option = document.createElement("option"); option.textContent = "没有可部署的已签名高版本镜像"; option.value = ""; select.append(option);
    }
    const active = state.otaDeployments.some((item) => item.device_id === device.device_id && ["queued", "offered", "transferring", "rebooting", "trial"].includes(item.status));
    const button = node("button", "", active ? "升级进行中" : "部署到设备");
    button.type = "button"; button.disabled = active || !artifacts.length;
    button.addEventListener("click", () => deployOta(device, Number(select.value)));
    actions.append(select, button); card.append(actions); deviceGrid.append(card);
  });
  if (!deviceGrid.children.length) deviceGrid.append(node("p", "control-hint", "设备第一次发送FW1 CHECK后会显示在这里。"));

  deploymentGrid.replaceChildren();
  state.otaDeployments.forEach((deployment) => {
    const card = node("article", "ota-deployment-card"), head = node("div", "ota-card-head"), title = node("div");
    title.append(node("b", "", `${deployment.from_version} → ${deployment.target_version}`), node("small", "", deployment.device_id));
    const badgeClass = deployment.status === "failed" || deployment.status === "rolled_back" ? "failed" : (["succeeded", "cancelled"].includes(deployment.status) ? "" : "busy");
    head.append(title, node("span", `ota-badge ${badgeClass}`, otaStateLabel(deployment.status))); card.append(head);
    const percent = deployment.bytes_total ? Math.min(100, Math.round(Number(deployment.bytes_sent) * 100 / Number(deployment.bytes_total))) : 0;
    const progress = node("div", "ota-progress"), bar = node("i"); bar.style.width = `${percent}%`; progress.append(bar); card.append(progress);
    const foot = node("div", "ota-deployment-foot");
    foot.append(node("span", "", `${percent}% · ${fileSize(deployment.bytes_sent)} / ${fileSize(deployment.bytes_total)}${deployment.error_code ? ` · ${deployment.error_code}` : ""}`));
    if (["queued", "offered"].includes(deployment.status)) {
      const cancel = node("button", "", "取消"); cancel.type = "button"; cancel.addEventListener("click", () => cancelOta(deployment.id)); foot.append(cancel);
    }
    card.append(foot); deploymentGrid.append(card);
  });
  if (!deploymentGrid.children.length) deploymentGrid.append(node("p", "control-hint", "暂无OTA部署任务。"));
}
async function deployOta(device, artifactId) {
  const accepted = await confirmAction("确认立即部署固件？", "设备将立即停止语音、制冷和风扇，写入非活动Bank并自动复位。");
  if (!accepted) return;
  try {
    await api("/api/admin/firmware/deployments", { method: "POST", body: { device_id: device.device_id, artifact_id: artifactId } });
    toast("OTA升级任务已排队"); await Promise.all([refreshOtaAdmin(), refreshAudit()]);
  } catch (error) { toast(error.message, "error"); }
}
async function cancelOta(deploymentId) {
  try {
    await api(`/api/admin/firmware/deployments/${deploymentId}/cancel`, { method: "POST" });
    toast("OTA任务已取消"); await refreshOtaAdmin();
  } catch (error) { toast(error.message, "error"); }
}
function renderFirmwareAdmin() {
  const grid = byId("adminFirmwareGrid"); grid.replaceChildren();
  state.firmware.forEach((release) => {
    const card = node("article", "admin-release-card"), head = node("div", "admin-release-head"), title = node("div"); title.append(node("h3", "", `${firmwareComponentLabel(release.component)} · ${release.version}`), node("small", "", `${release.published ? "已发布" : "草稿"} · ${release.is_current ? "当前版本" : release.channel}`));
    const actions = node("div", "admin-release-actions"), publish = node("button", "", release.published ? "下架" : "发布"), current = node("button", "", "设为当前"), remove = node("button", "danger", "删除"); [publish, current, remove].forEach((b) => b.type = "button"); publish.addEventListener("click", () => updateRelease(release.id, { published: !release.published })); current.addEventListener("click", () => updateRelease(release.id, { is_current: true })); remove.addEventListener("click", () => deleteRelease(release)); actions.append(publish, current, remove); head.append(title, actions); card.append(head, node("p", "control-hint", release.changelog || "暂无更新说明"));
    const artifacts = node("div", "admin-artifacts");
    (release.artifacts || []).forEach((artifact) => {
      const otaStatus = artifact.kind === "ota" ? ` · ${artifact.image_valid ? "ECDSA-P256已验签" : `不可部署:${artifact.validation_error || "未验证"}`}` : "";
      const row = node("div", "admin-artifact"), text = node("span", "", `${artifact.filename} · ${fileSize(artifact.size_bytes)} · ${artifact.downloads || 0}次下载${otaStatus}`), actions = node("div", "artifact-actions"), download = node("a", "mini-button", "下载"), del = node("button", "mini-button danger-text", "删除");
      download.href = `/api/public/firmware/artifacts/${artifact.id}/download`; del.type = "button"; del.addEventListener("click", () => deleteArtifact(artifact)); actions.append(download, del); row.append(text, actions); artifacts.append(row);
    });
    if (!artifacts.children.length) artifacts.append(node("p", "control-hint", "尚未上传文件")); card.append(artifacts);
    const upload = node("div", "upload-row"), kind = document.createElement("select");
    [["install", "安装文件"], ["ota", "安全OTA镜像"], ["source", "源码包"], ["model", "模型"], ["project", "工程包"]].forEach(([value, label]) => { const option = document.createElement("option"); option.value = value; option.textContent = label; kind.append(option); });
    const file = document.createElement("input"); file.type = "file"; const button = node("button", "", "上传文件"); button.type = "button"; button.addEventListener("click", () => uploadFirmware(release, kind.value, file)); upload.append(kind, file, button); card.append(upload); grid.append(card);
  });
  if (!grid.children.length) grid.append(node("p", "control-hint", "暂无固件版本，请先创建版本"));
}

async function createRelease(event) {
  event.preventDefault();
  try { await api("/api/admin/firmware/releases", { method: "POST", body: { component: byId("releaseComponent").value, version: byId("releaseVersion").value.trim(), title: byId("releaseTitle").value.trim(), channel: byId("releaseChannel").value, changelog: byId("releaseChangelog").value.trim(), published: byId("releasePublished").checked, is_current: byId("releaseCurrent").checked } }); event.target.reset(); toast("固件版本已创建"); await Promise.all([refreshFirmwareAdmin(), refreshAudit()]); } catch (error) { toast(error.message, "error"); }
}
async function updateRelease(id, fields) { try { await api(`/api/admin/firmware/releases/${id}`, { method: "PATCH", body: fields }); toast("版本状态已更新"); await refreshFirmwareAdmin(); } catch (error) { toast(error.message, "error"); } }
async function deleteRelease(release) { const accepted = await confirmAction("确认删除固件版本？", `将删除“${release.version}”及其文件记录，已下载到用户电脑的文件不受影响。`); if (!accepted) return; try { await api(`/api/admin/firmware/releases/${release.id}`, { method: "DELETE" }); toast("固件版本已删除"); await refreshFirmwareAdmin(); } catch (error) { toast(error.message, "error"); } }
async function deleteArtifact(artifact) { const accepted = await confirmAction("确认删除固件文件？", artifact.filename); if (!accepted) return; try { await api(`/api/admin/firmware/artifacts/${artifact.id}`, { method: "DELETE" }); toast("固件文件已删除"); await refreshFirmwareAdmin(); } catch (error) { toast(error.message, "error"); } }
async function uploadFirmware(release, kind, input) {
  const file = input.files && input.files[0]; if (!file) { toast("请先选择固件文件", "error"); return; } if (file.size > 256 * 1024 * 1024) { toast("文件不能超过256MB", "error"); return; }
  try { const query = new URLSearchParams({ release_id: String(release.id), kind, filename: file.name }); if (kind === "ota") { query.set("target_part", "R7FA6M5BF2CBG"); query.set("min_bootloader", "1.0.0+0"); } const response = await fetch(`/api/admin/firmware/artifacts?${query}`, { method: "POST", headers: { "Content-Type": "application/octet-stream", "X-CSRF-Token": state.csrf }, credentials: "same-origin", body: file }); let data = {}; try { data = await response.json(); } catch (_) {} if (!response.ok) throw new ApiError(data.validation_error ? `${data.error}：${data.validation_error}` : (data.error || `上传失败 (${response.status})`), response.status, data); input.value = ""; toast(data.deduplicated ? "文件已去重并关联到版本" : "固件上传成功"); await Promise.all([refreshFirmwareAdmin(), refreshOtaAdmin(), refreshAudit()]); } catch (error) { toast(error.message, "error"); }
}

function drawAdminMetricCharts() {
  drawAdminCanvas("adminPowerChart", "powerChartEmpty", [{ key: "power_40_w", color: "#1769e0" }, { key: "power_41_w", color: "#7969e8" }, { key: "power_total_w", color: "#e89a35" }], "W");
  drawAdminCanvas("adminNetworkChart", "networkChartEmpty", [{ key: "network_rtt_ms", color: "#1769e0" }, { key: "heartbeat_gap_ms", color: "#13aebf" }], "ms");
}
function drawAdminCanvas(canvasId, emptyId, series, unit) {
  const canvas = byId(canvasId), empty = byId(emptyId); if (!canvas) return; const rect = canvas.getBoundingClientRect(), ratio = Math.min(2, window.devicePixelRatio || 1); canvas.width = Math.max(1, Math.round(rect.width * ratio)); canvas.height = Math.max(1, Math.round(rect.height * ratio)); const context = canvas.getContext("2d"); context.setTransform(ratio, 0, 0, ratio, 0, 0); context.clearRect(0, 0, rect.width, rect.height); const values = []; state.history.forEach((point) => series.forEach((line) => { if (point[line.key] !== null && point[line.key] !== undefined) values.push(Number(point[line.key])); })); empty.style.display = values.length > 1 ? "none" : "grid"; if (values.length < 2) return; const padding = { left: 42, right: 14, top: 10, bottom: 24 }, width = rect.width - padding.left - padding.right, height = rect.height - padding.top - padding.bottom; let min = Math.max(0, Math.floor(Math.min(...values) * .9)), max = Math.ceil(Math.max(...values) * 1.1 + 1); if (max <= min) max = min + 1; const x = (index) => padding.left + index / Math.max(1, state.history.length - 1) * width, y = (value) => padding.top + (max - value) / (max - min) * height; context.font = "9px Segoe UI"; for (let tick = 0; tick <= 4; tick++) { const yy = padding.top + tick * height / 4; context.strokeStyle = "rgba(82,119,161,.12)"; context.beginPath(); context.moveTo(padding.left, yy); context.lineTo(padding.left + width, yy); context.stroke(); context.fillStyle = "#7a8da4"; context.fillText(`${(max - tick * (max - min) / 4).toFixed(0)}${unit}`, 2, yy + 3); } if (canvasId === "adminNetworkChart") { state.history.forEach((point, index) => { if (!point.online) { context.fillStyle = "rgba(223,93,111,.08)"; context.fillRect(x(index), padding.top, Math.max(2, width / state.history.length), height); } }); } series.forEach((line) => { context.strokeStyle = line.color; context.lineWidth = 2; context.beginPath(); let open = false; state.history.forEach((point, index) => { const raw = point[line.key]; if (raw === null || raw === undefined) { open = false; return; } if (!open) { context.moveTo(x(index), y(Number(raw))); open = true; } else context.lineTo(x(index), y(Number(raw))); }); context.stroke(); });
}

function bindEvents() {
  byId("loginForm").addEventListener("submit", handleLogin);
  byId("adminNav").addEventListener("click", handleAdminNavigation);
  window.addEventListener("popstate", () => setAdminRoute());
  byId("passwordToggle").addEventListener("click", () => {
    const input = byId("adminPassword");
    const visible = input.type === "text";
    input.type = visible ? "password" : "text";
    byId("passwordToggle").textContent = visible ? "显示" : "隐藏";
  });
  byId("logoutButton").addEventListener("click", handleLogout);
  byId("tempDown").addEventListener("click", () => sendControl({ set_temp: Math.max(-40, targetTemperature() - 1) }));
  byId("tempUp").addEventListener("click", () => sendControl({ set_temp: Math.min(80, targetTemperature() + 1) }));
  byId("tempRange").addEventListener("change", (event) => sendControl({ set_temp: Number(event.target.value) }));
  byId("tempInput").addEventListener("change", (event) => sendControl({ set_temp: Math.max(-40, Math.min(80, Number(event.target.value))) }));
  document.querySelectorAll("#fanControl button").forEach((button) => button.addEventListener("click", () => sendControl({ fan: Number(button.dataset.fan) })));
  byId("coolSwitch").addEventListener("change", (event) => sendControl({ cool: event.target.checked }));
  byId("waterSwitch").addEventListener("change", (event) => sendControl({ water_cool: event.target.checked }));
  byId("doorButton").addEventListener("click", async () => {
    const accepted = await confirmAction("确认发送开门指令？", "服务器会在下一次W800状态轮询中发送一次性开门命令。页面只能确认命令已下发，不能代替物理门锁反馈。");
    if (accepted) sendControl({ door: true });
  });
  byId("inventorySearch").addEventListener("input", (event) => { state.inventorySearch = event.target.value; renderInventory(); });
  document.querySelectorAll("#inventoryFilters button").forEach((button) => button.addEventListener("click", () => {
    state.inventoryFilter = button.dataset.filter;
    document.querySelectorAll("#inventoryFilters button").forEach((item) => item.classList.toggle("active", item === button));
    renderInventory();
  }));
  byId("medicineForm").addEventListener("submit", saveMedicine);
  byId("medicineClose").addEventListener("click", () => byId("medicineDialog").close());
  byId("medicineCancel").addEventListener("click", () => byId("medicineDialog").close());
  byId("deleteMedicine").addEventListener("click", deleteMedicine);
  document.querySelectorAll("#quantityActions [data-operation]").forEach((button) => button.addEventListener("click", () => quantityOperation(button.dataset.operation, Number(button.dataset.value))));
  byId("quantitySet").addEventListener("click", () => quantityOperation("set", Number(byId("medicineQuantity").value)));
  byId("healthForm").addEventListener("submit", submitHealth);
  byId("symptomInput").addEventListener("input", (event) => { byId("symptomCount").textContent = String(event.target.value.length); });
  byId("refreshAudit").addEventListener("click", refreshAudit);
  byId("clientCreateForm").addEventListener("submit", createClient);
  byId("copyToken").addEventListener("click", async () => {
    try {
      await navigator.clipboard.writeText(byId("generatedToken").textContent);
      toast("Token已复制，请立即保存");
    } catch (_) { toast("复制失败，请手工选中Token", "error"); }
  });
  byId("refreshLogs").addEventListener("click", refreshLogs);
  byId("logLevel").addEventListener("change", refreshLogs);
  byId("logQuery").addEventListener("keydown", (event) => { if (event.key === "Enter") refreshLogs(); });
  byId("powerSettingsForm").addEventListener("submit", savePowerSettings);
  byId("powerAlertFilter").addEventListener("change", refreshPowerAlerts);
  byId("ackPowerAlert").addEventListener("click", () => { if (state.activeAlertId) acknowledgePowerAlert(state.activeAlertId); });
  byId("releaseCreateForm").addEventListener("submit", createRelease);
  document.querySelectorAll("#rangeTabs button").forEach((button) => button.addEventListener("click", () => {
    state.historyRange = button.dataset.range;
    document.querySelectorAll("#rangeTabs button").forEach((item) => item.classList.toggle("active", item === button));
    refreshHistory();
  }));
  window.addEventListener("resize", () => window.requestAnimationFrame(() => { drawChart(); drawAdminMetricCharts(); }));
  window.setInterval(() => { byId("clock").textContent = new Date().toLocaleTimeString("zh-CN", { hour12: false }); }, 1000);
}

setAdminRoute();
bindEvents();
checkSession();
