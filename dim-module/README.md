# dim —— 设备标识管理（KernelSU 模块）

在用户态统一管理 Serial、Android ID（SSAID）、OAID、DRM ID、AppSetID，
带一个本地 Web 面板（`http://127.0.0.1:16667/`）。

- 每个标识都有 `原值 / 当前 / 目标 / 状态`，写盘前备份、写后校验、失败回滚
- 支持按应用单独重置 Android ID，支持一键更换全部标识
- 面板可从 KernelSU 模块卡片打开，卡片上的「执行」按钮等于「一键更换全部标识」

---

## 适用环境

| 项 | 说明 |
|---|---|
| Root 方案 | **KernelSU**（依赖 `ksud`、`resetprop`）。Magisk 未适配 |
| 架构 | arm64（aarch64） |
| Android | 11 及以上（依赖 `abx2xml` / `xml2abx` 处理 ColorOS 的 ABX 格式；普通 XML 也能处理） |

### 内核兼容性

| 内核 | KMI | 对应 ko 文件名 |
|---|---|---|
| 6.6（Android 15） | `android15-6.6` | `ko/android15-6.6.ko` 或 `ko/6.6.ko` |
| 6.12（Android 16） | `android16-6.12` | `ko/android16-6.12.ko` 或 `ko/6.12.ko` |

`bin/ko.sh` 会按 `uname -r` 推出 KMI，再校验 ko 里的 `vermagic=`：
内核版本号与 `android<X>` 标签都要对得上才会 insmod，否则跳过并在日志里说明
（避免把 6.6 的 ko 塞进 6.12 的内核）。

用户态部分（Serial / SSAID / OAID / 面板）与内核版本无关，两个内核通用。
6.12 起 LSM 改为 per-hook 静态调用、`security_hook_list` 去掉 hlist 节点，
这部分由内核模块的 `>= 6.12` 分支处理，需要单独编译对应的 ko。

| DRM ID | 需要配套内核模块 `susfs_guard_lkm`（见本仓库 `kernel/`），提供 `/proc/susfs_drm_guard`。**没有它时该功能会显示「未就绪」，其余功能不受影响** |
| OAID | 仅 ColorOS / OxygenOS 等含 `com.heytap.openid` 服务的 ROM。其他 ROM 上 OAID 只能读缓存、不能轮换，按钮会提示不可用 |
| 应用图标 | 需要 proot 环境里的 python + PIL + aapt。没有时列表仍可用，图标退化为首字母占位 |

---

## 安装

### 方式一：KernelSU 管理器安装 zip（推荐）

下载 `dim-vX.Y.Z.zip` → KernelSU 管理器 → 模块 → 从本地安装 → 重启。

### 方式二：手动放置

1. 把压缩包内容解到 `/data/adb/modules/dim/`（`module.prop` 必须在第一层）
2. 确认可执行位：`chmod 0755 bin/*.sh bin/dim *.sh webroot/cgi-bin/api`
3. 重启，或手动 `sh /data/adb/modules/dim/bin/httpd_ctl.sh start`

安装时脚本会：
- 把 `config.conf.example` 复制成 `/data/adb/dim/config.conf`（已存在则不动）
- 记录原始 Serial 到 `state/original.json`
- 若安装目录里存在 `com.tencent.tmgp.dfm`（三角洲行动），把 `SSAID_PKGS` / `DRM_PKGS` 默认设成它；没装则留空

---

## 首次使用

1. 打开 KernelSU 模块卡片 → 点 `<>`（或卡片上的执行按钮）打开面板
2. 面板顶部「总开关」打开 → 会按 `config.conf` 应用 Serial / Android ID / DRM ID
3. 在「安卓 ID」卡片点 `+` 选择目标应用，然后点右上角「随机」
4. **重启**，Android ID / OAID 才会被应用读到（Serial 与 DRM 立即生效）

一键更换全部标识：`action=randomize` 或模块卡片的 ▶ 按钮，
会重生成 Serial / SSAID / OAID / DRM ID、重置 AppSetID，
并按配置 force-stop 目标应用、清理 QIMEI 与 OAID 缓存。

---

## 配置（`/data/adb/dim/config.conf`）

```ini
AUTO_APPLY=1        # 开机自动应用（还需 MASTER=1）
MASTER=1            # 总开关
SERIAL=random       # keep / random / 16 位 hex
ANDROID_ID=keep     # keep / random
OAID=keep           # keep / rotate
DRM=keep            # keep / random / 64 位 hex
SSAID_PKGS=com.tencent.tmgp.dfm          # 安卓 ID 作用的应用（逗号分隔）
DRM_PKGS=com.tencent.tmgp.dfm            # DRM 作用的应用（按包名，换机也能用）
DRM_UIDS=                                # 由包名自动换算，通常不用手填
DRM_APPLY=1         # 0 = dry-run 只观察不写
DRM_AUTO=1          # 开机自动下发 DRM
DRM_HALUID=1013     # 固定 media uid，**不要改成 0**
STOP_AFTER=1        # 更换后 force-stop 目标应用
CLEAR_QIMEI=1       # 更换后清 QIMEI 缓存
DRM_STOP=1          # 改 DRM 后 force-stop 生效应用
HTTP_PORT=16667
```

改完可直接 `sh /data/adb/modules/dim/bin/dim apply`，或在面板里关再开总开关。

---

## 命令行

```sh
dim status              # 全量状态 JSON
dim randomize           # 一键更换全部标识
dim ssaid <pkg[,pkg]>   # 重置指定应用的 Android ID
dim ssaid-restore <pkg> # 还原指定应用（* = 整体还原）
dim serial 1234...      # 指定 Serial
dim drmid <64hex>       # 指定 DRM ID
dim oaid                # 轮换 OAID（并清理目标应用缓存）
dim restore             # 还原可还原项并关闭总开关
dim log                 # 最近 200 行日志
```

---

## 常见问题

**面板一直「检测中…」**
面板通过 `http://127.0.0.1:16667` 访问；KSU WebUI 的页面来源是 `https://mui.kernelsu.org`，
属于跨域，CGI 需要对白名单来源回显 `Access-Control-Allow-Origin`。若自己换了 WebUI 外壳，
把来源域名加进 `webroot/cgi-bin/api` 的 case 分支即可。

**OAID 显示「系统未持久化，无真值」/ 「还原」灰着**
ColorOS 的 OAID 由 `com.heytap.openid` 运行时下发，磁盘上没有真值副本，所以只能轮换、不能还原。
轮换后模块会自动 force-stop 并清掉目标应用的 OAID 缓存，应用下次冷启动才拿到新值。

**改了 Android ID 没效果**
`settings_ssaid.xml` 由 system_server 掌管，运行时会用内存副本回写。必须重启。

**DRM 面板显示「未就绪」**
说明 `susfs_guard_lkm` 内核模块没加载：`cat /proc/susfs_drm_guard` 不存在。
先装配套内核模块；`DRM_HALUID` 保持在 1013。

**DRM ID 改了，应用里看不到变化？**

每次修改 DRM ID（随机 / 手动指定 / 一键更换）都会**自动重载一次内核模块**
（rmmod + insmod，约 2 秒），不需要手动点重载。

如果应用仍显示旧值，说明它把 deviceUniqueId 缓存在自己进程里了：
`am force-stop <包名>` 或重启一次即可。

实测（6.6.118 + DevCheck）：重载后 DevCheck 显示的值与面板「当前」完全一致。

**应用列表空白**
`/data/adb/dim/apps.json` 损坏或为空。删掉后重新打开选择器（会重新生成），
或手动 `sh /data/adb/modules/dim/bin/icons.sh`。

---

## 卸载

卸载脚本会先执行 `dim restore-all`：还原 Serial、`settings_ssaid.xml`、
`settings_secure.xml`、`app_set_ids.xml`、`oaid_persistence_0`（存在的话），
关闭 DRM 钩子，然后删除 `/data/adb/dim`。

**卸载后请重启**，让 system_server 重新读盘。

---

## 安全提示

- 面板只监听 `127.0.0.1`，但 CGI **没有鉴权**：同一台设备上的任意应用、以及浏览器里打开的任意网页
  （`fetch(..., {mode:'no-cors'})` 简单 GET）都能触发写操作。请勿安装来路不明的应用，
  也不要在手机浏览器里乱点陌生页面。
- 模块会修改系统标识文件，属于高风险操作。**先确保会用 `restore`，再开总开关**。
- 本模块不联网，不上传任何数据；日志只落在 `/data/adb/dim/state/daemon.log`。

---

## 目录结构

```
module.prop              模块信息
customize.sh             安装期：依赖检查、默认配置、记录原始 Serial
post-fs-data.sh          开机前：重放 Serial、写 legacy android_id
service.sh               开机后：刷新 SSAID 样本、补齐 DRM uid、下发 DRM、起 httpd
action.sh                模块卡片「执行」= 一键更换全部标识
uninstall.sh             卸载前：restore-all + 停 httpd
bin/dim                  后端全部动作
bin/httpd_ctl.sh         面板 httpd 单例管理（探活 / 锁）
bin/ko.sh                按内核主次版本加载 ko
bin/icons.sh             应用图标缓存（临时目录生成，成功才替换）
bin/apps.sh              应用列表（无图标版）
bin/appicon.py           proot 内运行的图标提取器
bin/oaid-runtime.jar     OAID 轮换（app_process + OPLUS 服务）
ko/<kver>.ko             可选：内核模块副本，不随 zip 分发
webroot/                 面板：index.html、app.js、cgi-bin/api
```

状态与配置都在模块目录之外：

```
/data/adb/dim/config.conf          配置
/data/adb/dim/state/               运行状态、备份、日志
/data/adb/dim/apps.json            应用列表缓存
```

---

## 分发 / 打包

推 tag `dim-v*`（或手动触发 workflow）会跑 `.github/workflows/dim-zip.yml`：
语法检查 → 按 module.prop 的 version 打包 → `dim-<version>-arm64.zip`
（module.prop 在压缩包第一层，脚本保持 0755）→ 上传 Releases。

zip 里不含 `ko/` 与 `webroot/icons/`：
- `ko/` 由配套内核模块提供（`/proc/susfs_drm_guard` 已存在时 dim 不需要自己 insmod）
- 图标是运行时生成的缓存

本地手动打包：

```sh
cd dim-module
chmod 0755 *.sh bin/*.sh bin/dim webroot/cgi-bin/api
zip -r ../dim-$(sed -n 's/^version=//p' module.prop)-arm64.zip . -x '*.bak*' -x '.git*'
```
