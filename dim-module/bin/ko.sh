#!/system/bin/sh
# 加载内核模块
#   规则照抄内核拦截模块（hideguard/susfs_boot.sh）：ko/<X.Y>.ko 优先、带锁、
#   已加载就跳过、只校验 vermagic 主次版本，不搞太多花活。
MODDIR=${0%/*}
MODDIR=${MODDIR%/bin}
LOG=/data/adb/dim/state/daemon.log
STATE=/data/adb/dim/state
LOCK="$STATE/ko.lock"
mkdir -p "$STATE" 2>/dev/null

log() { printf '[%s] ko: %s\n' "$(date '+%H:%M:%S')" "$*" >> "$LOG" 2>/dev/null; }

CHECK=0
[ "$1" = "check" ] && CHECK=1

# 已经加载（不管是 dim 还是内核拦截模块加载的）就直接返回
if [ "$CHECK" != "1" ] && [ -d /sys/module/susfs_guard_lkm ]; then
  log "already loaded"
  echo loaded
  exit 0
fi

REL=$(uname -r 2>/dev/null)
KMM=$(printf '%s' "$REL" | cut -d. -f1,2)

# 选 ko：ko/<X.Y>.ko 优先；其次 ko/ 下任何以 <X.Y>.ko 结尾的（兼容 android15-6.6.ko 之类）
KO=""
if [ -n "$KMM" ] && [ -f "$MODDIR/ko/$KMM.ko" ]; then
  KO="$MODDIR/ko/$KMM.ko"
else
  for f in "$MODDIR/ko"/*"$KMM".ko "$MODDIR/ko"/*.ko; do
    [ -f "$f" ] || continue
    KO="$f"
    break
  done
fi

# 并发锁：post-fs-data 和手动 dim ko 可能撞车（没有 flock 就跳过）
if [ "$CHECK" != "1" ] && command -v flock >/dev/null 2>&1; then
  exec 9>"$LOCK" 2>/dev/null && flock 9 2>/dev/null
fi

if [ -z "$KO" ]; then
  log "no ko for kernel $REL (ko dir: $(ls "$MODDIR/ko" 2>/dev/null | tr '\n' ' '))"
  echo "no-ko"
  exit 1
fi

# vermagic 只比主次版本（6.6 / 6.12），patch 级别允许不同：
# 同一 KMI 内 ABI 稳定，release 串由 susfs_insmod 就地改写
VM=$(grep -a -o 'vermagic=[0-9][^ ]*' "$KO" 2>/dev/null | head -1 | cut -d= -f2)
VMM=$(printf '%s' "$VM" | cut -d. -f1,2)
if [ -n "$VMM" ] && [ "$VMM" != "$KMM" ]; then
  log "REFUSE: $(basename "$KO") vermagic=$VM, kernel=$KMM"
  echo "vermagic-mismatch"
  exit 1
fi

if [ "$CHECK" = "1" ]; then
  printf 'ko=%s\nvermagic=%s\nrelease=%s\n' "$KO" "${VM:-unknown}" "$REL"
  exit 0
fi

log "loading $(basename "$KO") for $REL"

if [ -x /data/adb/ksu/bin/ksud ]; then
  out=$(/data/adb/ksu/bin/ksud insmod "$KO" 2>&1)
elif [ -x "$MODDIR/bin/susfs_insmod" ]; then
  out=$(sh "$MODDIR/bin/susfs_insmod" "$KO" 2>&1)
else
  log "no loader available"
  echo no-loader
  exit 1
fi
log "insmod out: $out"

if [ -d /sys/module/susfs_guard_lkm ]; then
  log "loaded ok"
  echo loaded
else
  log "load FAILED"
  echo failed
  exit 1
fi