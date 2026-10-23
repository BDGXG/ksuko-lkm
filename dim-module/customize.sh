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

# 依赖自检：缺什么就明说，别让人装完以为全都能用
ui_print "- 检查依赖"
if [ -x /data/adb/ksu/bin/ksud ]; then
  ui_print "  KernelSU        OK"
else
  ui_print "  ! 未找到 /data/adb/ksu/bin/ksud，resetprop / insmod 可能不可用"
fi
if [ -x /data/adb/ksu/bin/resetprop ] || [ -x "$MODPATH/bin/resetprop" ]; then
  ui_print "  resetprop       OK"
else
  ui_print "  ! 没有 resetprop，Serial 伪装不可用（其他功能正常）"
fi
if [ -e /proc/susfs_drm_guard ]; then
  ui_print "  DRM 内核节点     OK"
else
  ui_print "  ! 未检测到 /proc/susfs_drm_guard"
  ui_print "    DRM ID 功能需要配套内核模块 susfs_guard_lkm，其余功能不受影响"
fi
if [ -e /data/system/openid_config.xml ] || [ -d /data/data/com.heytap.openid ]; then
  ui_print "  OPLUS OAID       OK"
else
  ui_print "  ! 本机没有 com.heytap.openid，OAID 轮换将不可用"
fi

# 默认目标应用：三角洲行动。只有本机真的装了才设为默认
CFG=/data/adb/dim/config.conf
DEFPKG=""
pm list packages 2>/dev/null | grep -qx "package:com.tencent.tmgp.dfm" && DEFPKG=com.tencent.tmgp.dfm
[ -n "$DEFPKG" ] && ui_print "- 默认目标应用: $DEFPKG"
for k in SSAID_PKGS DRM_PKGS; do
  [ -n "$DEFPKG" ] || break
  if grep -q "^$k=" "$CFG" 2>/dev/null; then
    [ -n "$(sed -n "s/^$k=//p" "$CFG" 2>/dev/null | tail -1)" ] \
      || sed -i "s|^$k=.*|$k=$DEFPKG|" "$CFG"
  else
    printf '%s=%s\n' "$k" "$DEFPKG" >> "$CFG"
  fi
done

# 首次记录原始 Serial
if [ ! -f /data/adb/dim/state/original.json ]; then
  printf '{"serial":"%s"}\n' "$(getprop ro.serialno)" > /data/adb/dim/state/original.json
fi

ui_print "- 完成。命令: sh /data/adb/modules/dim/bin/dim status"
ui_print "- 打开配置: /data/adb/dim/config.conf"