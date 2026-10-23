#!/system/bin/sh
# KSU 模块卡片「执行」：一键更换全部标识
# 等同面板里的「一键更换全部标识」（action=randomize）：
#   Serial / Android ID(SSAID) / OAID / DRM ID / AppSetID 全部重生成，
#   并按配置停掉目标应用、清 QIMEI 与 OAID 缓存。
MODDIR=${0%/*}
CONF=/data/adb/dim/config.conf
PORT=$(sed -n 's/^HTTP_PORT=//p' "$CONF" 2>/dev/null | tail -1)
[ -n "$PORT" ] || PORT=16667
APPLY=$(sed -n 's/^DRM_APPLY=//p' "$CONF" 2>/dev/null | tail -1)

# 顺手保证面板在跑（换完就能直接点开看）
sh "$MODDIR/bin/httpd_ctl.sh" start >/dev/null 2>&1

echo "正在更换全部标识…"
out=$(sh "$MODDIR/bin/dim" randomize 2>&1)

if printf '%s' "$out" | grep -q '"ok":false'; then
  echo ""
  echo "失败：$out"
  exit 1
fi

ser=$(printf '%s' "$out" | sed -n 's/.*"serial":{"original":"[^"]*","current":"\([^"]*\)".*/\1/p' | head -1)
drm=$(printf '%s' "$out" | sed -n 's/.*"virtual_id":"\([^"]*\)".*/\1/p' | head -1)
[ -n "$ser" ] || ser=$(getprop ro.serialno)

echo ""
echo "已更换："
echo "  Serial   $ser"
if [ -n "$drm" ]; then
  echo "  DRM ID   $(printf '%s' "$drm" | cut -c1-16)…"
  [ "$APPLY" = "1" ] || echo "           （DRM_APPLY=0，仅观察未写入）"
fi
echo ""
echo "Android ID / OAID 已重置，重启后生效"
echo "面板： http://127.0.0.1:$PORT/"