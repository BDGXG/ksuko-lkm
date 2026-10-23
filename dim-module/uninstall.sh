#!/system/bin/sh
# 卸载：完全还原（Serial + SSAID + AppSetID + OAID + 关 DRM 钩子）
# 顺序很重要：先把能还原的都还原，再删配置目录。
MODDIR=${0%/*}

if [ -x "$MODDIR/bin/dim" ]; then
  out=$(sh "$MODDIR/bin/dim" restore-all 2>&1)
  case "$out" in
    *'"ok":true'*) echo "dim: restore-all ok" ;;
    *)             echo "dim: restore-all FAILED: $out" ;;
  esac
fi

# 兜底：万一 restore-all 没跑到，至少把 Serial 和 DRM 关掉
STATE=/data/adb/dim/state
P=/data/adb/ksu/bin/resetprop
[ -x "$P" ] || P="$MODDIR/bin/resetprop"
SER=$(sed -n 's/.*"serial"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$STATE/original.json" 2>/dev/null | head -1)
if [ -n "$SER" ] && [ -x "$P" ]; then
  for k in ro.serialno ro.boot.serialno ro.vendor.serialno ro.vendor.oem.serialno \
           ro.kernel.androidboot.serialno persist.sys.serialno ro.ril.oem.sno; do
    "$P" -n "$k" "$SER" 2>/dev/null
  done
fi
[ -e /proc/susfs_drm_guard ] && echo "enable 0" > /proc/susfs_drm_guard 2>/dev/null

sh "$MODDIR/bin/httpd_ctl.sh" stop 2>/dev/null
rm -rf /data/adb/dim