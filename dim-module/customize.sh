#!/system/bin/sh
SKIPUNZIP=0

ui_print "- 安装 设备标识管理 (dim)"
ui_print "- 架构: $(uname -m)"

[ "$(uname -m)" = "aarch64" ] || abort "! 仅支持 arm64"

chmod 0755 "$MODPATH/bin/dim" 2>/dev/null
chmod 0755 "$MODPATH/post-fs-data.sh" "$MODPATH/service.sh" "$MODPATH/uninstall.sh" 2>/dev/null
[ -f "$MODPATH/bin/oaid-runtime.jar" ] && chmod 0644 "$MODPATH/bin/oaid-runtime.jar"
[ -d "$MODPATH/webroot" ] && chmod -R 0644 "$MODPATH/webroot"

# 配置与状态放在模块目录之外，升级不丢
mkdir -p /data/adb/dim/state
chmod 700 /data/adb/dim
if [ ! -f /data/adb/dim/config.conf ]; then
  cp -f "$MODPATH/config.conf.example" /data/adb/dim/config.conf 2>/dev/null
  chmod 600 /data/adb/dim/config.conf
fi

# 首次记录原始 Serial
if [ ! -f /data/adb/dim/state/original.json ]; then
  printf '{"serial":"%s"}\n' "$(getprop ro.serialno)" > /data/adb/dim/state/original.json
fi

ui_print "- 完成。命令: sh /data/adb/modules/dim/bin/dim status"
ui_print "- 打开配置: /data/adb/dim/config.conf"