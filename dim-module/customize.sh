#!/system/bin/sh
SKIPUNZIP=0

ui_print "- 安装 设备标识管理 (dim)"
ui_print "- 架构: $(uname -m)"

[ "$(uname -m)" = "aarch64" ] || abort "! 仅支持 arm64"

# 权限：目录必须带 x 位，CGI 必须可执行
#   注意不要用 chmod -R 0644 webroot：会把目录和 cgi-bin/api 的 x 位一起抹掉，
#   结果是 httpd 找不到 CGI，面板永远「检测中」（实测过）
chmod 0755 "$MODPATH" "$MODPATH/bin" "$MODPATH/ko" \
           "$MODPATH/webroot" "$MODPATH/webroot/cgi-bin" 2>/dev/null
chmod 0755 "$MODPATH"/*.sh "$MODPATH/bin"/*.sh "$MODPATH/bin/dim" \
           "$MODPATH/bin/susfs_insmod" "$MODPATH/webroot/cgi-bin/api" 2>/dev/null
chmod 0644 "$MODPATH/module.prop" "$MODPATH/config.conf.example" "$MODPATH/README.md" \
           "$MODPATH/bin/appicon.py" "$MODPATH/bin/oaid-runtime.jar" \
           "$MODPATH/webroot/index.html" "$MODPATH/webroot/app.js" 2>/dev/null
for _k in "$MODPATH/ko"/*.ko; do [ -f "$_k" ] && chmod 0644 "$_k"; done

# 配置与状态放在模块目录之外，升级不丢
mkdir -p /data/adb/dim/state
chmod 700 /data/adb/dim
if [ ! -f /data/adb/dim/config.conf ]; then
  cp -f "$MODPATH/config.conf.example" /data/adb/dim/config.conf 2>/dev/null
  chmod 600 /data/adb/dim/config.conf
fi

# 总开关：面板已去掉这个开关，老配置缺键时补成 1
CFG0=/data/adb/dim/config.conf
if [ -f "$CFG0" ]; then
  if grep -q '^MASTER=' "$CFG0" 2>/dev/null; then
    sed -i 's/^MASTER=.*/MASTER=1/' "$CFG0"
  else
    printf 'MASTER=1
' >> "$CFG0"
  fi
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

# 内核模块：只提示不阻断（只影响 DRM，Serial/SSAID/OAID 用不到它）
KMM_I=$(uname -r 2>/dev/null | cut -d. -f1,2)
if [ -n "$KMM_I" ] && [ -f "$MODPATH/ko/$KMM_I.ko" ]; then
  ui_print "  内核模块 ko/$KMM_I.ko   OK"
else
  ui_print "  ! 没有 ko/${KMM_I:-未知}.ko，DRM ID 功能不可用（其余功能正常）"
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