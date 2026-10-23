#!/system/bin/sh
# 加载内核模块（按内核主次版本选 ko），已加载则跳过
MODDIR=${0%/*}
MODDIR=${MODDIR%/bin}
LOG=/data/adb/dim/state/daemon.log
mkdir -p /data/adb/dim/state 2>/dev/null

log() { printf '[%s] ko: %s\n' "$(date '+%H:%M:%S')" "$*" >> "$LOG" 2>/dev/null; }

[ -d /sys/module/susfs_guard_lkm ] && { log "already loaded"; exit 0; }

REL=$(uname -r)
KMM=$(echo "$REL" | cut -d. -f1,2)
KO="$MODDIR/ko/$KMM.ko"
[ -f "$KO" ] || KO="$MODDIR/ko/6.6.ko"
[ -f "$KO" ] || { log "ko missing for $REL"; exit 1; }

if [ -x /data/adb/ksu/bin/ksud ]; then
  out=$(/data/adb/ksu/bin/ksud insmod "$KO" 2>&1)
  log "ksud insmod $(basename "$KO"): $out"
elif [ -x "$MODDIR/bin/susfs_insmod" ]; then
  out=$(sh "$MODDIR/bin/susfs_insmod" "$KO" 2>&1)
  log "susfs_insmod: $out"
else
  log "no loader available"
  exit 1
fi

[ -d /sys/module/susfs_guard_lkm ] && log "loaded ok" || log "load FAILED"
