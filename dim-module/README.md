# dim-module —— 设备标识管理（用户态模块）

这是手机上运行中的 `dim` 模块源码快照，用于归档与换机重装。

## 目录

```
module.prop          模块信息
service.sh           开机后：刷新 SSAID 样本、下发 DRM、起 httpd
post-fs-data.sh      开机前：重放 serial、写 legacy android_id
dim-module/bin/dim       主脚本（所有动作的后端）
bin/httpd_ctl.sh     面板 httpd 单例管理（探活/锁）
bin/ko.sh            按内核主次版本加载 ko
bin/icons.sh         应用图标缓存
bin/apps.sh          应用列表
webroot/             面板（index.html + app.js + cgi-bin/api）
```

## 安装

1. 把整个目录放到 `/data/adb/modules/dim/`
2. 确认 `/data/adb/dim/config.conf` 存在（可从 `config.conf.example` 复制）
3. 重启，或手动 `sh bin/httpd_ctl.sh start`

## 关键配置

```ini
MASTER=1          # 总开关
DRM_PKGS=         # 目标应用（包名，逗号分隔；换设备也能用）
DRM=              # 虚拟 DRM ID（64 hex）
DRM_APPLY=1
DRM_AUTO=1        # 开机自动下发
DRM_HALUID=1013   # 固定，勿改 0
DRM_STOP=1        # 改 DRM 后 force-stop 生效应用
STOP_AFTER=1      # 一键更换后 force-stop 目标应用
CLEAR_QIMEI=1     # 一键更换后清 QIMEI 缓存
```

## 依赖

- KernelSU（提供 `/data/adb/ksu/bin/resetprop`、`ksud insmod`）
- 内核模块 `susfs_guard_lkm`（提供 `/proc/susfs_drm_guard`）
- busybox（httpd、xxd、base64）

## 注意

- `DRM_HALUID` 必须保持 `1013`，设成 `0` 会让钩子扫描全系统 binder，实测卡死
- 改 DRM ID 后必须重启 Widevine HAL 才会生效（脚本已自动做）
- Android ID（SSAID）需重启设备才被应用读到
- 详细原理与踩坑记录见 `docs/drm-guard.md`、`docs/oaid-notes.md`、`docs/dim-integration.md`
