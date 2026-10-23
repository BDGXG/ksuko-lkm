#!/system/bin/sh
# 加载内核模块：按内核版本挑 ko（KMI 名优先），并校验 vermagic，避免把错版本的 ko 塞进去
MODDIR=${0%/*}
MODDIR=${MODDIR%/bin}
LOG=/data/adb/dim/state/daemon.log
mkdir -p /data/adb/dim/state 2>/dev/null

log() { printf '[%s] ko: %s\n' "$(date '+%H:%M:%S')" "$*" >> "$LOG" 2>/dev/null; }

CHECK=0
[ "$1" = "check" ] && CHECK=1

# 已经被别的东西（比如 susfs_guard_lkm 模块自己）加载就不用管
if [ "$CHECK" != "1" ] && [ -d /sys/module/susfs_guard_lkm ]; then
  log "already loaded (by other module)"
  echo loaded
  exit 0
fi

REL=$(uname -r)
KMM=$(printf '%s' "$REL" | cut -d. -f1,2)
ANDROID=$(printf '%s' "$REL" | sed -n 's/.*android\([0-9][0-9]*\).*/\1/p')

# 候选顺序：android<X>-<major.minor>.ko → <major.minor>.ko → ko/ 下任意 *.ko
CANDS=""
[ -n "$ANDROID" ] && CANDS="$CANDS $MODDIR/ko/android${ANDROID}-${KMM}.ko"
CANDS="$CANDS $MODDIR/ko/${KMM}.ko"
for f in "$MODDIR/ko"/*.ko; do [ -f "$f" ] && CANDS="$CANDS $f"; done

vm_of() { grep -ao 'vermagic=[^ ]*' "$1" 2>/dev/null | head -1 | sed 's/^vermagic=//'; }

pick_ko() {
  for k in $CANDS; do
    [ -f "$k" ] || continue
    vm=$(vm_of "$k")
    [ -n "$vm" ] || { log "skip $(basename "$k"): no vermagic"; continue; }
    # GKI 在同一 KMI 内 ABI 稳定，patch 级别可以不同（6.6.118 上跑 6.6.127 编的 ko，
    # vermagic 由 susfs_insmod 改写）。这里只比 major.minor + android 标签。
    vmver=${vm%%-*}
    case "$vmver" in
      "$KMM"|"$KMM".*) ;;
      *) log "skip $(basename "$k"): vermagic $vmver not in $KMM.x"; continue ;;
    esac
    case "$vm" in
      *android*)
        if [ -z "$ANDROID" ]; then log "skip $(basename "$k"): host release has no android tag"; continue; fi
        printf '%s' "$vm" | grep -aq "android${ANDROID}" \
          || { log "skip $(basename "$k"): android tag mismatch (vm=$vm)"; continue; }
        ;;
    esac
    printf '%s\n' "$k"; return 0
  done
  return 1
}

KO=$(pick_ko)
if [ -z "$KO" ]; then
  log "no matching ko for $REL; candidates:$CANDS"
  echo "no-matching-ko"
  exit 1
fi
log "picked $(basename "$KO") for $REL"

if [ "$CHECK" = "1" ]; then
  printf 'ko=%s\nvermagic=%s\nrelease=%s\n' "$KO" "$(vm_of "$KO")" "$REL"
  exit 0
fi

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

if [ -d /sys/module/susfs_guard_lkm ]; then
  log "loaded ok"
  echo loaded
else
  log "load FAILED"
  echo failed
  exit 1
fi