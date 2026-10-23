#!/system/bin/sh
# post-fs-data：Serial 属性必须在这里写，否则 zygote 起来后读到的是旧值
MODDIR=${0%/*}
CONF_DIR=/data/adb/dim
STATE="$CONF_DIR/state"
LOG="$STATE/daemon.log"
mkdir -p "$STATE" 2>/dev/null

sh "$MODDIR/bin/ko.sh" 2>/dev/null

log() { printf '[%s] boot: %s\n' "$(date '+%H:%M:%S')" "$*" >> "$LOG" 2>/dev/null; }

[ "$(sed -n 's/^AUTO_APPLY=//p' "$CONF_DIR/config.conf" 2>/dev/null | tail -1)" = "1" ] || exit 0
[ "$(sed -n 's/^MASTER=//p' "$CONF_DIR/config.conf" 2>/dev/null | tail -1)" = "1" ] || exit 0

# 上次应用过的目标值
SER=$(sed -n 's/.*"serial"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$STATE/identity.json" 2>/dev/null | head -1)

P=/data/adb/ksu/bin/resetprop
[ -x "$P" ] || P="$MODDIR/bin/resetprop"

if [ -n "$SER" ] && [ -x "$P" ]; then
  for k in ro.serialno ro.boot.serialno ro.vendor.serialno ro.vendor.oem.serialno \
           ro.kernel.androidboot.serialno persist.sys.serialno ro.ril.oem.sno; do
    "$P" -n "$k" "$SER" 2>>"$LOG"
  done
  log "serial re-applied: $SER"
fi

# 3) 应用待生效的全局 Android ID
#    必须在 framework 启动前写：系统运行期间 system_server 会用内存副本回写 settings_secure.xml，
#    把运行时的修改覆盖掉（已实测）。
T="$STATE/ssaid.target"
if [ -f "$T" ]; then
  nv=$(head -1 "$T" 2>/dev/null)
  f=/data/system/users/0/settings_secure.xml
  if [ -n "$nv" ] && [ -f "$f" ]; then
    wf=""
    if [ "$(head -c 3 "$f" 2>/dev/null)" = "ABX" ]; then
      /system/bin/abx2xml "$f" "$f.xml" 2>>"$LOG" && wf="$f.xml"
    else
      wf="$f"
    fi
    if [ -n "$wf" ]; then
      awk -v new="$nv" '
        /<setting / && index($0, "name=\"android_id\"") > 0 {
          sub(/value="[^"]*"/, "value=\"" new "\"");
          sub(/defaultValue="[^"]*"/, "defaultValue=\"" new "\"");
        }
        { print }' "$wf" > "$wf.tmp"
      if [ "$(head -c 3 "$f" 2>/dev/null)" = "ABX" ]; then
        if /system/bin/xml2abx "$wf.tmp" "$f.abx" 2>>"$LOG"; then
          cp -f "$f.abx" "$f"; chmod 0600 "$f"; chown system:system "$f" 2>/dev/null
          /system/bin/restorecon "$f" 2>/dev/null
          log "legacy android_id applied at boot: $nv"
        else
          log "legacy android_id xml2abx FAILED"
        fi
        rm -f "$f.xml" "$f.abx"
      else
        cp -f "$wf.tmp" "$f"; chmod 0600 "$f"; chown system:system "$f" 2>/dev/null
        /system/bin/restorecon "$f" 2>/dev/null
        log "legacy android_id applied at boot: $nv"
      fi
      rm -f "$wf.tmp"
    fi
  fi
  rm -f "$T"
fi