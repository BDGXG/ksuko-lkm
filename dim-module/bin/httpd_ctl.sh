#!/system/bin/sh
# httpd 单例管理（借鉴 hideguard/daemon_http.sh）
#   - 目录锁串行化，避免 service.sh / action.sh 并发起两个
#   - 探活：curl 127.0.0.1:PORT 返回 200 才算健康
#   - 按 /proc/<pid>/cmdline 的可执行路径匹配，不误杀其他模块的 httpd
MODDIR=${0%/*}; MODDIR=${MODDIR%/bin}
PORT=$(sed -n 's/^HTTP_PORT=//p' /data/adb/dim/config.conf 2>/dev/null | tail -1)
[ -n "$PORT" ] || PORT=16667
BIN=/data/adb/ksu/bin/busybox
[ -x "$BIN" ] || BIN=/system/bin/busybox
LOG=/data/adb/dim/state/daemon.log
LOCK=/data/adb/dim/.httpd.lock
ROOT="$MODDIR/webroot"

log(){ printf '[%s] httpd: %s\n' "$(date '+%H:%M:%S')" "$*" >> "$LOG" 2>/dev/null; }

pids(){ for d in /proc/[0-9]*; do [ -r "$d/cmdline" ] || continue
  c=$( { tr '\\0' ' ' < "$d/cmdline"; } 2>/dev/null )
  case "$c" in *dim/webroot*|*"-h $ROOT"*) echo "${d#/proc/}";; esac
 done }
cnt(){ pids | wc -l | tr -d ' '; }
healthy(){ [ "$(/system/bin/curl -s -m 2 -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/" 2>/dev/null)" = "200" ]; }

start(){
  mkdir -p "/data/adb/dim/state"
  if healthy && [ "$(cnt)" = "1" ]; then log "healthy, skip"; return 0; fi
  for p in $(pids); do kill -9 "$p" 2>/dev/null; done
  sleep 0.3
  "$BIN" httpd -p 127.0.0.1:$PORT -h "$ROOT" 2>>"$LOG"
  sleep 0.5
  if healthy; then log "started on $PORT"; echo "$PORT" > /data/adb/dim/state/port; return 0
  else log "start FAILED"; return 1; fi
}

stop(){ for p in $(pids); do kill -9 "$p" 2>/dev/null; done; log "stopped"; }
status(){ echo "port=$PORT procs=$(cnt) healthy=$(healthy && echo 1 || echo 0)"; }

case "$1" in
  start) exec 9>"$LOCK"; flock 9 2>/dev/null; start ;;
  stop) stop ;;
  status) status ;;
  *) echo "usage: $0 start|stop|status"; exit 2 ;;
esac
