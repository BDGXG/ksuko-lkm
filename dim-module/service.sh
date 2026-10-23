#!/system/bin/sh
# service.sh：boot_completed 之后的下发/收尾
MODDIR=${0%/*}
CONF_DIR=/data/adb/dim
STATE="$CONF_DIR/state"
LOG="$STATE/daemon.log"
DRM_PROC=/proc/susfs_drm_guard
mkdir -p "$STATE" 2>/dev/null

log() { printf '[%s] svc: %s\n' "$(date '+%H:%M:%S')" "$*" >> "$LOG" 2>/dev/null; }
conf_get() { sed -n "s/^$1=//p" "$CONF_DIR/config.conf" 2>/dev/null | tail -1; }

until [ "$(getprop sys.boot_completed)" = "1" ]; do sleep 2; done
sleep 2
# 先把面板拉起来，后面的基线/DRM 下发即使慢也不影响开面板
sh "$MODDIR/bin/httpd_ctl.sh" start >/dev/null 2>&1
sleep 4

# 1) 一次性标记
[ -f "$STATE/pending_reboot" ] && rm -f "$STATE/pending_reboot" "$STATE/ssaid.target" && log "pending_reboot cleared"

# 2) 刷新 Android ID 当前值样本（面板按应用 SSAID 显示，而不是 legacy 值）
_ssaid_f=/data/system/users/0/settings_ssaid.xml
_ssaid_src=""
if [ -f "$_ssaid_f" ]; then
  if [ "$(head -c 3 "$_ssaid_f" 2>/dev/null)" = "ABX" ]; then
    /system/bin/abx2xml "$_ssaid_f" /data/local/tmp/.sc.xml 2>/dev/null && _ssaid_src=/data/local/tmp/.sc.xml
  else
    _ssaid_src="$_ssaid_f"
  fi
  if [ -n "$_ssaid_src" ]; then
    sed -n 's/.*name="[0-9][0-9]*" value="\([^"]*\)".*/\1/p' "$_ssaid_src" 2>/dev/null | head -1 > "$STATE/ssaid.current"
    log "ssaid.current refreshed: $(cat "$STATE/ssaid.current" 2>/dev/null)"
    sh "$MODDIR/bin/dim" ssaid-map >/dev/null 2>&1
  fi
  rm -f /data/local/tmp/.sc.xml
fi

sh "$MODDIR/bin/ko.sh" 2>/dev/null

# 1.5) 原值基线：首次开机就把「当前值」存成原值快照，否则面板只能显示未分配，
#      直到你手动改一次 ID 才生成备份（这就是「刷完看不到原值」的原因）
[ -s "$STATE/baseline.json" ] || sh "$MODDIR/bin/dim" baseline-init >/dev/null 2>&1
[ -s "$STATE/original.json" ]  || printf '{"serial":"%s"}
' "$(getprop ro.serialno)" > "$STATE/original.json" 2>/dev/null
if [ ! -f "$BACKUP/ssaid.0.xml" ] && [ -f /data/system/users/0/settings_ssaid.xml ]; then
  cp -p /data/system/users/0/settings_ssaid.xml "$BACKUP/ssaid.0.xml" 2>/dev/null \
    && log "ssaid baseline snapshot taken"
fi
sh "$MODDIR/bin/dim" ssaid-map >/dev/null 2>&1

# 2) DRM 目标 uid 补齐：安装/迁移后 DRM_UIDS 可能为空，按包名重算一次
if [ -n "$(conf_get DRM_PKGS)" ] && [ -z "$(conf_get DRM_UIDS)" ]; then
  sh "$MODDIR/bin/dim" drmpkgs "$(conf_get DRM_PKGS)" >/dev/null 2>&1
  log "drm uids resolved from pkgs: $(conf_get DRM_UIDS)"
fi

# 2) DRM 下发（hideguard 的内核节点）
# identity.json 丢了但内核已有 id：落盘一份，保证面板能显示目标值
if [ ! -s "$STATE/identity.json" ] && [ -e "$DRM_PROC" ]; then
  _did=$(sed -n 's/^id=//p' "$DRM_PROC" 2>/dev/null | head -1)
  [ -n "$_did" ] && printf '{"serial":"","drmid":"%s"}\n' "$_did" > "$STATE/identity.json" 2>/dev/null \
    && log "identity.json restored from kernel id"
fi

if [ -e "$DRM_PROC" ] && [ "$(conf_get DRM_AUTO)" = "1" ]; then
  id=$(sed -n 's/.*"drmid"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$STATE/identity.json" 2>/dev/null | head -1)
  uids=$(conf_get DRM_UIDS)
  [ -n "$id" ]   && echo "id $id" > "$DRM_PROC"
  [ -n "$uids" ] && echo "uids $uids" > "$DRM_PROC"
  h=$(conf_get DRM_HALUID); [ -n "$h" ] && echo "haluid $h" > "$DRM_PROC"
  [ "$(conf_get DRM_APPLY)" = "1" ] && echo "apply 1" > "$DRM_PROC" || echo "apply 0" > "$DRM_PROC"
  echo "enable 1" > "$DRM_PROC"
  log "drm pushed: id=$id uids=$uids apply=$(conf_get DRM_APPLY)"
elif [ ! -e "$DRM_PROC" ]; then
  log "drm node absent (hideguard 未加载?)"
fi

# 3) OAID 开机补一次（部分 OPLUS 机型需要）
if [ "$(conf_get OAID)" = "rotate" ] && [ -f "$MODDIR/bin/oaid-runtime.jar" ]; then
  CLASSPATH="$MODDIR/bin/oaid-runtime.jar" /system/bin/app_process \
    -Djava.class.path="$MODDIR/bin/oaid-runtime.jar" /system/bin OplusOaidRuntime probe >>"$LOG" 2>&1
  log "oaid probe done"
fi


# 3.5) 应用列表：apps.json 缺失就先用 pm 快列表顶上，图标放后台慢慢生成
if [ ! -s /data/adb/dim/apps.json ]; then
  sh "$MODDIR/bin/apps.sh" >/dev/null 2>&1
  # 图标不在这里生成：proot 提取会占满 IO，导致开机后第一次打开面板卡十几秒。
  # 面板打开时会自己 kickIcons() 后台补（见 index.html）
  log "apps.json seeded (icons will be extracted on demand)"
fi

# 4) 启动本地 HTTP 面板（单例 + 探活，见 bin/httpd_ctl.sh）
sh "$MODDIR/bin/httpd_ctl.sh" start
log "httpd: $(sh "$MODDIR/bin/httpd_ctl.sh" status 2>/dev/null)"
log "service done"