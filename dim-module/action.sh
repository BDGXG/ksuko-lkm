#!/system/bin/sh
# KSU 模块卡片「执行」：重启面板并回显地址与状态
MODDIR=${0%/*}
PORT=$(sed -n 's/^HTTP_PORT=//p' /data/adb/dim/config.conf 2>/dev/null | tail -1)
[ -n "$PORT" ] || PORT=16667

sh "$MODDIR/bin/httpd_ctl.sh" start >/dev/null 2>&1

echo "设备标识管理"
echo "面板： http://127.0.0.1:$PORT/"
echo ""
sh "$MODDIR/bin/httpd_ctl.sh" status 2>/dev/null
echo ""
if [ -e /proc/susfs_drm_guard ]; then
  echo "DRM 内核节点：已就绪"
else
  echo "DRM 内核节点：不存在（内核模块未加载）"
fi
