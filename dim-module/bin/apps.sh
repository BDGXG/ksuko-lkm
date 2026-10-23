#!/system/bin/sh
# 生成应用列表缓存 -> /data/adb/dim/apps.json
OUT=/data/adb/dim/apps.json
TMP="$OUT.tmp"
mkdir -p /data/adb/dim
printf '[' > "$TMP"
# 应用名/包名要进 JSON，先转义（应用名里可能有引号或反斜杠）
esc() { printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' | tr -d '\r\n'; }

first=1
for p in $(pm list packages -3 2>/dev/null | sed 's/package://'); do
  u=$(stat -c %u "/data/data/$p" 2>/dev/null)
  [ -n "$u" ] || u=0
  [ $first -eq 1 ] || printf ',' >> "$TMP"
  printf '{"pkg":"%s","uid":%s,"label":"%s"}' "$(esc "$p")" "$u" "$(esc "$p")" >> "$TMP"
  first=0
done
printf ']' >> "$TMP"
mv -f "$TMP" "$OUT"
wc -c < "$OUT"
