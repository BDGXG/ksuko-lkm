"use strict";
const API = ["http://127.0.0.1:16667/cgi-bin/api", "/cgi-bin/api"];
const $ = id => document.getElementById(id);
let pending = null, pickMode = "ssaid", apps = [], appMap = {}, appsByPkg = {}, targets = [], rest = {}, kern = {};

const toast = t => {
  const e = $("toast"); e.textContent = t; e.classList.add("show");
  clearTimeout(window.__t); window.__t = setTimeout(() => e.classList.remove("show"), 1800);
};
const esc = s => String(s == null ? '' : s)
  .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
  .replace(/["']/g, function (c) {
    return ['&', c === '"' ? 'quot' : '#39', ';'].join('');
  });

// 从可能的脏输出中截取 JSON（后端偶有 native 工具把结果打到 stdout）
function pickJson(t) {
  const s = String(t == null ? "" : t);
  const a = s.search(/[[{]/);
  if (a < 0) return null;
  const b = Math.max(s.lastIndexOf("}"), s.lastIndexOf("]"));
  return b > a ? s.slice(a, b + 1) : null;
}

async function api(action, value) {
  let q = "?action=" + encodeURIComponent(action);
  if (value !== undefined && value !== null) q += "&value=" + encodeURIComponent(value);
  let err = null;
  for (const base of API) {
    let r;
    try { r = await fetch(base + q, { cache: "no-store" }); }
    catch (e) { err = e; continue; }
    if (!r.ok) { err = new Error("HTTP " + r.status); continue; }
    const j = pickJson(await r.text());
    // 服务器已经响应过：即使内容异常也不再换 base 重试，避免非幂等动作被执行两次
    if (!j) throw new Error("非 JSON 响应");
    return j;
  }
  throw err || new Error("接口不可用");
}

const IDS = [["serial","序列号"],["androidid","安卓 ID"],["oaid","广告标识"],["drmid","DRM ID"]];

async function loadApps(force) {
  if (apps.length && !force) return;
  try {
    const v = JSON.parse(await api("apps-cached"));
    apps = Array.isArray(v) ? v : [];
  } catch (e) { apps = []; }
  appMap = {}; appsByPkg = {};
  apps.forEach(a => { if (a && a.pkg) { appMap[String(a.uid)] = a; appsByPkg[a.pkg] = a; } });
}

async function doStatus() {
  let s;
  try { s = await api("status"); }
  catch (e) {
    toast("后台未响应");
    const d = $("devline");
    if (d) d.textContent = "后台未响应：" + e.message + "（127.0.0.1:16667）";
    return;
  }
  let d;
  try { d = JSON.parse(s); } catch (e) { return; }
  await loadApps();

  const de = d.device || {}, drm = d.drm || {}, ids = d.identities || {}, cfg = d.config || {};
  kern = d.kernel || {};
  const surf = {};
  (d.surfaces || []).forEach(s => { if (s && s.id) surf[s.id] = s; });

  // 哪些项的值来自系统/应用缓存（非实时读取）
  const isCached = k => {
    const s = surf[k];
    return !!(s && /cache/.test(String(s.detail || "")));
  };
  rest = d.restorable || {};
  const gr = d.global_reset || {};
  $("devline").textContent = (de.brand||"?") + " " + (de.model||"?") + " · " + (de.serial||"");
  const on = !!(drm.node && drm.enabled === "1");
  $("dot").classList.toggle("on", on);
  $("master").checked = !!cfg.master_enabled;
  if ($("optStop")) $("optStop").checked = cfg.stop_after === "1";
  if ($("optQimei")) $("optQimei").checked = cfg.clear_qimei === "1";
  if ($("optDrmStop")) $("optDrmStop").checked = cfg.drm_stop === "1";

  $("list").innerHTML = IDS.map(function (kv) {
    const k = kv[0], n = kv[1], it = ids[k] || {};
    const orig = it.original || "", cur = it.current || "", tgt = it.target || "";
    const st = it.state || "unknown";
    const badge = st === "applied" ? '<span class="bdg ok">已写入</span>'
      : (st === "pending" ? '<span class="bdg pd">待应用</span>' : '<span class="bdg">未设置</span>');
    const cg = isCached(k) ? '<span class="cgtag" title="读自系统/应用缓存，非实时">缓存值</span>' : '';
    const nr = (rest[k] === false) ? '<span class="nrtag" title="本机没有可回滚凭据">不可还原</span>' : '';
    const rb = (k === "androidid" && gr.reboot_required) ? '<span class="rbtag">待重启</span>' : '';
    const sup = SUPPORT[k] || [];
    const hbtn = (kind, label, danger) =>
      '<button class="hdbtn' + (danger ? ' danger' : '') + '"' +
      (sup.indexOf(kind) < 0 ? ' disabled' : ' data-k="' + k + '" data-a="' + kind + '" onclick="cardAct(this.dataset.k,this.dataset.a)"') +
      '>' + label + '</button>';
    const act = '<span class="hdact">' + hbtn('gen', '随机') + hbtn('clr', '还原') + '</span>';
    const row = (label, v, cls) =>
      '<div class="irow"><span class="k">' + label + '</span>' +
      '<span class="v ' + (cls || '') + '">' +
        (v ? esc(v) : (k === 'oaid' && label === '原值' ? '—（系统未持久化，无真值）' : '—')) + '</span>' +
      (v ? '<button class="copybtn" data-v="' + esc(v) + '" onclick="copy(this.dataset.v)">复制</button>' : '<span class="copybtn ghost"></span>') +
      '</div>';
    return '<div class="idcard">' +
      '<div class="ihd"><b>' + n + '</b>' + badge + cg + nr + rb + act + '</div>' +
      (k === "androidid" ? ''
        : (k === "oaid" ? row('当前', cur) : row('原值', orig) + row('当前', cur))) +
      cardTools(k) +
      '</div>';
  }).join("");

  targets = String(drm.pkgs || "").split(",").filter(Boolean);
  renderTargets();
  renderDrmChips();
  ssaidPkgs = String(cfg.ssaid_pkgs || "").split(",").filter(Boolean);
  ssaidMap = d.ssaid_map || [];
  renderPkgChips();
  if (drm.virtual_id) $("in-drm").value = drm.virtual_id;

  // —— 概览网格 / 状态胶囊（新版 UI，元素不存在时自动跳过）——
  const masterOn = !!cfg.master_enabled;
  const setTxt = (id, v) => { const e = $(id); if (e) e.textContent = v; };
  setTxt("mState", masterOn ? "已开启" : "已关闭");
  setTxt("mTargets", targets.length ? (targets.length + " 个") : "全部");
  setTxt("mDrm", on ? "已启用" : "未启用");
  let applied = 0;
  IDS.forEach(function (kv) { const it = ids[kv[0]] || {}; if (it.state === "applied") applied++; });
  setTxt("mIds", applied + " / " + IDS.length);
  const pill = $("statusPill");
  if (pill) pill.classList.toggle("ok", on);
  setTxt("statusText", on ? "已生效" : "未启用");
  const curOf = (id, k) => { const it = ids[k] || {}; setTxt(id, it.current || "未设置"); };
  curOf("curSerial", "serial");
  curOf("curSsaid", "androidid");
  curOf("curDrm", "drmid");
  if ($("drmStat")) doDrmStats();
}

function renderTargets() {
  const box = $("targets");
  if (!box) return;                 // 02 区的「作用于」列表已移除，这里直接跳过
  if (!targets.length) {
    box.innerHTML = '<div class="line"><span class="v" style="text-align:left">全部应用（未限定）</span></div>';
    if ($("tgtcount")) $("tgtcount").textContent = "全部应用";
    return;
  }
  if ($("tgtcount")) $("tgtcount").textContent = targets.length + " 个应用";
  box.innerHTML = targets.map(function (p) {
    const a = appsByPkg[p];
    const label = a ? (a.label || a.pkg) : p;
    const icon = (a && a.icon)
      ? '<img src="icons/' + a.pkg + '.png" style="width:34px;height:34px;border-radius:9px;flex:0 0 auto">'
      : '<img style="width:34px;height:34px;border-radius:9px;flex:0 0 auto;background:var(--line)">';
    return '<div class="line" style="min-height:54px">' + icon +
      '<span style="flex:1;min-width:0;padding-left:2px"><b style="display:block;font-size:14px;font-weight:600;overflow:hidden;text-overflow:ellipsis;white-space:nowrap">' + esc(label) + '</b></span>' +
      '<button class="del" onclick="removeTarget(\'' + p + '\')">删除</button></div>';
  }).join("");
}

async function saveTargets() {
  try {
    await api("drmpkgs", targets.join(","));
    await api("drm");
    toast("已更新目标应用");
  } catch (e) { toast("保存失败：" + e.message); }
  doStatus();
}
function removeTarget(uid) {
  targets = targets.filter(x => x !== uid);
  saveTargets();
}

function copy(t) {
  if (!t || t === "—") return;
  if (navigator.clipboard && navigator.clipboard.writeText) {
    navigator.clipboard.writeText(t).then(() => toast("已复制"), () => toast("复制失败"));
  } else {
    const a = document.createElement("textarea");
    a.value = t; document.body.appendChild(a); a.select();
    try { document.execCommand("copy"); toast("已复制"); } catch (e) {}
    document.body.removeChild(a);
  }
}

function confirmDo(title, msg, action, value) {
  pending = { action: action, value: value };
  $("dlgTitle").textContent = title;
  $("dlgMsg").textContent = msg;
  $("mask").classList.add("show");
}

async function closeDlg(ok) {
  $("mask").classList.remove("show");
  if (!ok || !pending) return;
  const p = pending; pending = null;
  try {
    if (p.action === "__drm__") {
      if (p.value) await api("drmid", p.value);
      await api("drm");
      toast("已下发");
    } else if (p.action === "__ssaid_multi__") {
      const list = String(p.value || "").split(",").filter(Boolean);
      for (const pkg of list) await api("ssaid", pkg);
      toast("已重置 " + list.length + " 个应用");
    } else {
      const r = await api(p.action, p.value);
      if (r && r.ok === false) throw new Error(r.error || "操作失败");
      toast("完成");
    }
  } catch (e) { toast("失败：" + e.message); }
  doStatus();
}

function restoreMsg() {
  let m = "Serial、Android ID（需重启）、DRM ID 将回到原始值，同时关闭总开关、停止开机自动应用。";
  const bad = [];
  if (rest.oaid === false) bad.push("OAID");
  if (rest.appset === false) bad.push("AppSetID");
  if (bad.length) m += "\n\n注意：" + bad.join("、") + " 在本机没有可回滚的备份，不会被还原。";
  return m;
}
function confirmDoRestore() { confirmDo("恢复原样", restoreMsg(), "restore"); }

function toggleMaster(el) {
  const on = el.checked;
  el.checked = !on;
  const msg = on
    ? "将按 config.conf 重新应用 Serial、Android ID、DRM ID，并恢复开机自动应用。"
    : restoreMsg();
  confirmDo(on ? "开启设备标识管理" : "关闭设备标识管理", msg, on ? "apply" : "restore");
}

let pickSel = { pkg: new Set(), uid: new Set() };

async function openPicker(mode) {
  pickMode = mode;
  pickSel = { pkg: new Set(), uid: new Set() };
  if (mode === "multi" || mode === "drm") targets.forEach(p => pickSel.pkg.add(p));
  else ssaidPkgs.forEach(p => pickSel.pkg.add(p));
  $("pmask").classList.add("show");
  $("alist").innerHTML = "加载中…";
  await loadApps(true);          // 打开选择器时重取 apps.json，装/卸应用后不至于显示旧列表
  if (!apps.length) {
    try { apps = JSON.parse(await api("apps")); appMap = {}; apps.forEach(a => { appMap[String(a.uid)] = a; }); } catch (e) {}
  }
  renderPicker();
}

let pickTimer = null;
function queueFilter() {
  clearTimeout(pickTimer);
  pickTimer = setTimeout(renderPicker, 120);
}

function renderPicker() {
  const f = ($("pfilter").value || "").trim().toLowerCase();
  const list = apps.filter(a => !f || (a.pkg + " " + (a.label || "")).toLowerCase().indexOf(f) >= 0);
  const byUid = (pickMode === "multi" || pickMode === "drm");
  $("alist").innerHTML = list.length ? list.map(a => {
    const sel = pickSel.pkg.has(a.pkg);
    return '<div class="arow" data-pkg="' + a.pkg + '" data-uid="' + a.uid + '" onclick="pick(this.dataset.pkg,this.dataset.uid)">' +
      (a.icon ? '<img loading="lazy" decoding="async" src="icons/' + a.pkg + '.png" onerror="this.style.visibility=\'hidden\'">' : '<img>') +
      '<span class="p"><b>' + esc(a.label || a.pkg) + '</b><s>' + a.pkg + '</s></span>' +
      '<span class="ck' + (sel ? " on" : "") + '"></span></div>';
  }).join("") : '<div class="arow">没有匹配</div>';
}

function pick(pkg) {
  if (pickSel.pkg.has(pkg)) pickSel.pkg.delete(pkg); else pickSel.pkg.add(pkg);
  renderPicker();
}

function applyPicker() {
  if (pickMode === "multi" || pickMode === "drm") {
    targets = Array.from(pickSel.pkg);
    saveTargets();
  } else {
    ssaidPkgs = Array.from(pickSel.pkg);
    renderPkgChips();
    api("ssaidpkgs", ssaidPkgs.join(",")).catch(function(){});
    toast("已选 " + ssaidPkgs.length + " 个应用");
  }
  closePicker();
}

function closePicker() { $("pmask").classList.remove("show"); }

async function refreshApps() {
  $("alist").innerHTML = "扫描中，请稍候（约 10~30 秒）…";
  try {
    apps = JSON.parse(await api("apps"));
    appMap = {}; appsByPkg = {};
    apps.forEach(a => { if (a && a.pkg) { appMap[String(a.uid)] = a; appsByPkg[a.pkg] = a; } });
    renderPicker();
    toast("已刷新 " + apps.length + " 个");
  } catch (e) { $("alist").innerHTML = "刷新失败"; }
}

function setSerial() {
  const v = $("in-serial").value.trim();
  if (!v) return toast("请输入序列号");
  if (!/^[0-9A-Fa-f]{16}$/.test(v)) return toast("需要 16 位十六进制");
  confirmDo("写入 Serial", v, "serial", v);
}
async function applyDrmId() {
  const v = ($("in-drm").value || "").trim();
  if (v && !/^[0-9a-fA-F]{64}$/.test(v)) return toast("需 64 位十六进制");
  try {
    if (v) await api("drmid", v);
    await api("drm");
    toast("已下发");
  } catch (e) { toast("失败：" + e.message); }
  doStatus();
}

// 卡片右上角的「生成 / 清除」支持矩阵与动作映射
const SUPPORT = { serial: ["gen", "clr"], androidid: ["gen", "clr"], oaid: ["gen"], drmid: ["gen", "clr"] };
function cardTools(k) {
  if (k === "serial") return '<div class="cdtools">' +
    '<details class="mini"><summary>手动写入指定序列号</summary>' +
    '<div class="field" style="margin-top:10px"><label>16 位十六进制</label>' +
    '<input id="in-serial" placeholder="例如 A1B2C3D4E5F60718"></div>' +
    '<button class="btn btn-gray btn-sm btn-full" onclick="setSerial()">写入序列号</button>' +
    '</details></div>';
  if (k === "androidid") return '<div class="cdtools">' +
    '<div class="pkglist" id="pkgChip"></div>' +
    '<div class="hint" style="margin-top:8px">右上角「随机 / 还原」作用于已选应用；一个都没选则对全部应用生效。改完需重启。</div></div>';
  if (k === "oaid") return '<div class="cdtools">' +
    '<div class="hint" style="margin:0">本机 OAID 由 com.heytap.openid 运行时下发，磁盘上没有真值，所以只能轮换、不能指定值，也无法还原。' +
    '<br>轮换后会自动停掉并清理目标应用的 OAID 缓存（umeng / 穿山甲），应用下次冷启动才会拿到新值。</div></div>';
  if (k === "drmid") return '<div class="cdtools">' +
    '<div class="field"><label>虚拟 ID（64 位十六进制，改完自动下发）</label>' +
    '<input id="in-drm" placeholder="64 位十六进制" onchange="applyDrmId()"></div>' +
    '<div class="ihead"><div class="chips" id="drmChips"></div></div>' +
    '<div class="hint" id="drmStat">内核状态：—</div></div>';
}
// 应用小卡：只显示 图标 + 应用名（不显示包名）
// 只显示应用图标的大号 chip（点一下移除）
function iconChip(a, pkg, rmJs) {
  const label = a ? (a.label || a.pkg) : pkg;
  const ic = a && a.icon
    ? '<img src="icons/' + a.pkg + '.png" onerror="this.style.display=\'none\'">'
    : '<span class="noic">' + esc(label.charAt(0)) + '</span>';
  return '<span class="appico" title="' + esc(label) + '（点一下移除）" onclick="' + rmJs + '">' + ic + '</span>';
}
function appChip(a, pkg, uid, withDel) {
  const label = a ? (a.label || a.pkg) : (pkg || ("uid " + uid));
  const ic = a && a.icon
    ? '<img src="icons/' + a.pkg + '.png" onerror="this.style.display=\'none\'">'
    : '<span class="noic">' + esc(label.charAt(0)) + '</span>';
  const del = withDel
    ? '<i data-u="' + uid + '" onclick="rmDrmTarget(this.dataset.u)">×</i>' : '';
  return '<span class="appchip">' + ic + '<b>' + esc(label) + '</b>' + del + '</span>';
}

function addBtn(mode) {
  return '<i class="addbtn" onclick="openPicker(\'' + mode + '\')">+</i>';
}

function renderDrmChips() {
  const box = $("drmChips"); if (!box) return;
  box.innerHTML = targets.map(function (p) {
    return iconChip(appsByPkg[p], p, "rmDrmTarget('" + p + "')");
  }).join("") + addBtn("drm");
}

// 安卓 ID 选中的应用列表（内存态；空 = 全部）
let ssaidPkgs = [];
let ssaidMap = [];

function renderPkgChips() {
  const box = $("pkgChip"); if (!box) return;
  let html = "";
  ssaidPkgs.forEach(function (p) {
    const a = appsByPkg[p];
    const label = a ? (a.label || a.pkg) : p;
    const m = (ssaidMap || []).filter(function (x) { return x.pkg === p; })[0] || {};
    const ic = a && a.icon
      ? '<img src="icons/' + a.pkg + '.png" onerror="this.style.display=\'none\'">'
      : '<span class="noic">' + esc(label.charAt(0)) + '</span>';
    const rowv = function (k, v, cls) {
      const t = v || "未分配";
      return '<div class="pkgl"><span class="k">' + k + '</span>' +
        '<span class="v ' + (cls || '') + '">' + esc(t) + '</span>' +
        (v ? '<button class="copybtn" data-v="' + esc(v) + '" onclick="copy(this.dataset.v)">复制</button>' : '') +
        '</div>';
    };
    html += '<div class="pkgrow">' +
      '<div class="pkghd">' + ic + '<b>' + esc(label) + '</b>' +
      '<span class="pkgact">' +
        '<button class="hdbtn" data-p="' + esc(p) + '" onclick="ssaidOne(this.dataset.p,\'gen\')">随机</button>' +
        '<button class="hdbtn" data-p="' + esc(p) + '" onclick="ssaidOne(this.dataset.p,\'clr\')">还原</button>' +
      '</span>' +
      '<i data-p="' + esc(p) + '" onclick="rmSsaidPkg(this.dataset.p)">×</i></div>' +
      rowv("原值", m.orig) + rowv("当前", m.cur, "cur") +
      '</div>';
  });
  box.innerHTML = html + addBtn("ssaid");
}

function rmSsaidPkg(p) {
  ssaidPkgs = ssaidPkgs.filter(function (x) { return x !== p; });
  renderPkgChips();
  api("ssaidpkgs", ssaidPkgs.join(",")).catch(function(){});
}

function ssaidOne(pkg, kind) {
  if (kind === "gen")
    return confirmDo("随机安卓 ID", "只重置 " + pkg + "，需重启生效", "ssaid", pkg);
  return confirmDo("还原安卓 ID", "把 " + pkg + " 写回首次备份的安卓 ID，需重启生效", "ssaid-restore", pkg);
}

function rmDrmTarget(uid) {
  targets = targets.filter(function (x) { return x !== uid; });
  saveTargets();
}

function cardAct(k, kind) {
  if (k === "serial" && kind === "gen") return confirmDo("随机序列号", "按原值的长度与大小写随机生成并应用", "serial", "random");
  if (k === "serial" && kind === "clr") return confirmDo("还原序列号", "把序列号写回首次备份的真值", "serial-restore");
  if (k === "androidid" && kind === "gen") {
    if (ssaidPkgs.length)
      return confirmDo("随机安卓 ID", "重置选中的 " + ssaidPkgs.length + " 个应用，需重启生效", "ssaid", ssaidPkgs.join(","));
    return confirmDo("随机安卓 ID（全部应用）", "将重置所有应用的安卓 ID，需重启生效", "ssaid", "*");
  }
  if (k === "androidid" && kind === "clr") {
    if (ssaidPkgs.length)
      return confirmDo("还原安卓 ID", "把选中的 " + ssaidPkgs.length + " 个应用写回首次备份的原值，需重启生效", "ssaid-restore", ssaidPkgs.join(","));
    return confirmDo("还原安卓 ID（全部应用）", "用首次备份整体还原所有应用的安卓 ID，需重启生效", "ssaid-restore", "*");
  }
  if (k === "oaid" && kind === "gen") return confirmDo("随机广告标识", "调用 OPLUS 接口轮换广告标识（面板显示的是缓存值）", "oaid");
  if (k === "drmid" && kind === "gen") return confirmDo("随机 DRM ID", "生成新的 64 位虚拟 ID 并立即下发", "drmid-rand");
  if (k === "drmid" && kind === "clr") return confirmDo("还原 DRM ID", "关闭内核钩子，DRM ID 立即回到真值", "drm-off");
  toast("该项不支持此操作");
}

async function toggleOpt(kind, el) {
  const v = el.checked ? "1" : "0";
  try {
    await api(kind === "stop" ? "opt-stop" : (kind === "drmstop" ? "opt-drmstop" : "opt-qimei"), v);
    toast("已保存");
  } catch (e) {
    el.checked = !el.checked;
    toast("保存失败");
  }
}

async function doDrmStats() {
  const el = $("drmStat");
  try {
    const d = JSON.parse(await api("drm-stats"));
    const kp = kern && kern.release ? ("内核 " + String(kern.release).split("-")[0] + " · ") : "";
    if (!d.ok) { if (el) el.textContent = kp + "节点不存在（内核模块未加载）"; return; }
    const on = d.enabled === "1" && d.mode === "apply";
    if (el) el.textContent = kp + "内核：" + (on ? "生效中" : "未生效") + " · " + d.mode +
      " · armed=" + d.armed + " · 命中 " + d.exact_hits + " · 写入 " + d.written + " · 失败 " + d.nofault_fail;
  } catch (e) {
    if (el) el.textContent = "内核状态：读取失败";
  }
}

async function doDrmInfo() {
  try { const d = JSON.parse(await api("drm-info")); $("drmraw").textContent = d.raw || d.error || "(空)"; }
  catch (e) { $("drmraw").textContent = "读取失败"; }
}
async function doLog() {
  try { const d = JSON.parse(await api("log")); $("logbox").textContent = d.log || "(空)"; }
  catch (e) { $("logbox").textContent = "读取失败"; }
}

window.onerror = function (m, s, l) {
  try {
    var t = document.getElementById("statusText");
    if (t) t.textContent = "ERR";
    var pill = document.getElementById("statusPill");
    if (pill) pill.classList.remove("ok");
  } catch (e) {}
  return false;
};
doStatus().catch(function (e) {
  var d = document.getElementById("devline");
  if (d) d.textContent = "doStatus 失败：" + e.message;
});