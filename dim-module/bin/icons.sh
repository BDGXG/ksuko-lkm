#!/system/bin/sh
# 提取应用名+图标：复用 proot(python+PIL+aapt)，输出 apps.json 与 webroot/icons
# 规则：
#   1) 先生成到 stage 临时目录，成功后才覆盖 webroot/icons；失败则保留旧图标
#   2) proot 不存在时只重建 apps.json（label 回退为包名），完全不动图标目录
M=${0%/*}; M=${M%/bin}
OUT=/data/adb/dim
STAGE="$OUT/.icons.new"
LIST=/data/local/tmp/.dim_list.tsv
PR=/data/user/0/com.ai.assistance.operit/files/usr/bin/proot
RB=$(dirname "$PR")/../var/lib/proot-distro/installed-rootfs/ubuntu

esc() { printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' | tr -d '\r\n'; }

mkdir -p "$OUT" "$M/webroot/icons" 2>/dev/null
# 只清临时目录，不碰现役图标
[ -d "$STAGE" ] && find "$STAGE" -mindepth 1 -delete >/dev/null 2>&1
mkdir -p "$STAGE/icons" 2>/dev/null

: > "$LIST"
for p in $(pm list packages -3 2>/dev/null | sed 's/package://'); do
  a=$(pm path "$p" 2>/dev/null | head -1 | sed 's/package://')
  [ -n "$a" ] || continue
  case "$a" in /data/app/*) ;; *) continue ;; esac
  printf '%s\t%s\n' "$p" "$a" >> "$LIST"
done

MODE=keep
if [ -x "$PR" ] && [ -d "$RB" ]; then
  sed -e "s#/data/local/tmp/.hg_list.tsv#$LIST#" \
      -e "s#/data/user/0/com.ai.assistance.operit/files/.hg_out#$STAGE#" \
      "$M/bin/appicon.py" > /data/local/tmp/.dim_icon.py 2>/dev/null

  BINDS=""
  for d in /my_preload /my_stock /my_product /my_heytap /my_bigball /my_carrier \
           /my_company /my_doc /my_engineering /my_manifest /my_version \
           /preload /special_preload /odm /product /system /system_ext /vendor; do
    [ -d "$d" ] && BINDS="$BINDS -b $d"
  done

  "$PR" -0 -r "$RB" --link2symlink \
    -b /dev -b /proc -b /sys -b /dev/pts \
    -b /data -b /data/local/tmp \
    -b /data/user/0/com.ai.assistance.operit \
    $BINDS \
    -w /root \
    /usr/bin/env -i HOME=/root PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    /usr/bin/python3 /data/local/tmp/.dim_icon.py >/dev/null 2>&1

  if [ -n "$(ls -A "$STAGE/icons" 2>/dev/null)" ]; then
    cp -f "$STAGE/icons"/*.png "$M/webroot/icons"/ 2>/dev/null
    # 成功生成后才清理「本次结果里已不存在」的旧图标
    for f in "$M/webroot/icons"/*.png; do
      [ -f "$f" ] || continue
      b=$(basename "$f")
      [ -f "$STAGE/icons/$b" ] || rm -f "$f"
    done
    MODE=fresh
  fi
fi

printf '[' > "$OUT/apps.json.tmp"
first=1
for p in $(awk -F'\t' '{print $1}' "$LIST" 2>/dev/null); do
  u=$(stat -c %u "/data/data/$p" 2>/dev/null); [ -n "$u" ] || u=0
  lab=$(awk -F'\t' -v k="$p" '$1==k{print $2}' "$STAGE/meta.tsv" 2>/dev/null)
  [ -n "$lab" ] || lab="$p"
  ic=0; [ -f "$M/webroot/icons/$p.png" ] && ic=1
  [ $first -eq 1 ] || printf ',' >> "$OUT/apps.json.tmp"
  printf '{"pkg":"%s","label":"%s","uid":%s,"icon":%s}' "$(esc "$p")" "$(esc "$lab")" "$u" "$ic" >> "$OUT/apps.json.tmp"
  first=0
done
printf ']' >> "$OUT/apps.json.tmp"
mv -f "$OUT/apps.json.tmp" "$OUT/apps.json"

find "$STAGE" -mindepth 1 -delete >/dev/null 2>&1
echo "done mode=$MODE icons=$(ls -A "$M/webroot/icons" 2>/dev/null | wc -l | tr -d ' ')"