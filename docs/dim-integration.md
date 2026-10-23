# dim 模块 × drm_guard 对接契约

`dim`（设备标识管理，用户态）与 `drm_guard`（内核模块）之间只有一个接口：
`/proc/susfs_drm_guard`。本文把这条接口固定下来，任一侧改动都要同步这里。

## 1. 节点命令（写入）

| 命令 | 作用 | 备注 |
|---|---|---|
| `id <64hex>` | 设定虚拟 deviceUniqueId | 必须 64 位 hex |
| `uids <uid[,uid…]>` | 目标应用白名单 | 为空/0 表示不匹配任何应用 |
| `haluid <uid>` | HAL 侧作用域 | **固定 1013**；`0` 会关闭作用域限制，实测导致卡死，禁止 |
| `apply 0\|1` | 0 = dry-run，只计数不写；1 = 真改写 | 关闭功能首选 `apply 0` |
| `enable 0\|1` | 启用/停用钩子 | `enable 0` 会摘掉 kretprobe，`enable 1` 无法重新挂上，只能重载 ko —— 避免使用 |

## 2. 节点输出（只读，`drm-stats` 解析）

```
mode=apply enabled=1 armed=1 probe=binder_ioctl
 uids=1 haluid=1013
  uid[0]=10440
id=<64hex>
calls=… read_streams=… reply_seen=…
exact_hits=… written=… nofault_fail=…
w_cand=… w_written=…
```

一行可能包含多个 `key=value`，解析时按空格切分再按 `=` 分组（dim 的 `drm_stats()` 已这样做）。

**生效判据：`written`（或 `w_written`）必须 > 0。** `exact_hits` 只能说明命中。

## 3. dim 侧配置键

```ini
DRM=<64hex>        # 虚拟 ID；identity.json 里的 drmid 优先
DRM_UIDS=10440     # 目标应用 uid
DRM_APPLY=1        # 0 = dry-run
DRM_AUTO=1         # 开机自动下发
DRM_HALUID=1013    # 固定，不要改
```

## 4. dim 侧实现点

| 位置 | 作用 |
|---|---|
| `bin/dim drm_push()` | 依次写 id/uids/haluid/apply/enable，然后**回读 `id=` 做校验**，不一致则记日志并返回失败 |
| `bin/dim drm_off()` | 只写 `apply 0` + `uids 0`，保留 kretprobe |
| `bin/dim drm_stats()` | 读取并输出 JSON，供面板与 CLI 使用 |
| `service.sh` | boot_completed 后，若 `DRM_AUTO=1` 则执行一次 `drm` 下发 |
| 面板「DRM ID」卡 | 生成 = 随机 64hex 并下发；清除 = 关闭钩子；状态行 = `drm-stats` |

## 5. 版本要求

ko 必须包含 `drm_u_write()` 的 `copy_to_user` 回退（见 `CHANGELOG.md` 2026-10-23），
即产物大小约 `2653216` 字节、分支 `fix-uwrite`。旧 ko（`2650904`）在缺页时写入必定失败。

## 6. 常见故障对应

| 现象 | 原因 |
|---|---|
| 节点不存在 | ko 未加载（看 `disable`、`ko.sh` 日志） |
| `enabled=0 / probe=off` | ko 加载了但未 `enable 1`（或写 `uids 0` 后未恢复） |
| `exact_hits` 涨、`written` 恒 0 | 旧 ko，没有缺页回退 |
| 面板值与 DevCheck 不一致 | mediadrm 缓存；以 `written` 计数为准 |
