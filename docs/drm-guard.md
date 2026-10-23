# drm_guard —— Widevine `deviceUniqueId` 伪装实现说明

本文件记录 `kernel/drm_guard.c` 的设计取舍、已验证结论与已知陷阱，避免后来者重复踩坑。

## 1. 目标

在**不改动 Widevine HAL 二进制、不碰内核结构体**的前提下，让指定进程通过
`MediaDrm.getPropertyByteArray("deviceUniqueId")` 读到伪造的 32 字节设备 ID。

## 2. 两条路径

| 路径 | 挂点 | 写入对象 | 状态 |
|---|---|---|---|
| A | `kretprobe binder_ioctl`，扫描 `BINDER_WRITE_READ` 的 read/write buffer | HAL 侧**用户态** parcel | ✅ 采用 |
| B | `kretprobe binder_transaction` + `binder_alloc_new_buf`，调用 `binder_alloc_copy_to_buffer` | binder **内核缓冲** | ⚠️ 实验性，曾导致硬挂起，默认关闭（`kwrite=0`） |

B 线的钩子挂在 binder 核心路径上，一旦状态机判断有误就可能死锁或写坏事务，代价是整机硬挂起（无 panic、无 pstore 记录）。**除非有充分验证环境，不要把 `kwrite` 打开。**

## 3. 匹配规则

- 请求：`BC_TRANSACTION`，`binder_transaction_data.code == 11`（`getPropertyByteArray`），parcel 内含 UTF-16 的 `deviceUniqueId`
- 回包：`status == 0` 且字节数组长度恰为 32
- 只处理 `data_size == 40`（4 字节 status + 4 字节长度 + 32 字节数据）的 parcel

## 4. 作用域（重要）

节点 `/proc/susfs_drm_guard` 的 `haluid` 是 **uid**，不是 tgid。

- `haluid 1013` —— 只匹配 media uid（Widevine HAL），正常取值
- `haluid 0` —— 源码语义是 *observe every process*，即**取消作用域限制，扫描全系统 binder 流量**

实测把 `haluid` 设为 0 会导致 `calls` 在数秒内涨到数千，系统随后卡死重启。
**生产中禁止使用 0。**

`uids` 则是目标应用白名单（如 `10440` = DevCheck），只有这些进程的请求/回包会被处理。

## 5. 已知陷阱

### 5.1 用户页未驻留导致写入失败（已修）

A 线写入原先只用 `copy_to_user_nofault()`。该函数**不触发缺页**，而 binder 刚返回的
parcel 用户页往往尚未 fault-in，于是必然返回 `-EFAULT`：

```
exact_hits=4  written=0  nofault_fail=8      ← 典型症状
```

修复：`nofault` 失败后回退到会走缺页处理的 `copy_to_user()`：

```c
if (copy_to_user_nofault(dst, src, n) && copy_to_user(dst, src, n)) {
        atomic64_inc(&g_stats.nofault_fail);
        return -EFAULT;
}
```

修复后 `written` 正常增长，DevCheck 能读到伪造值。

### 5.2 mediadrm 服务缓存

该服务会缓存 `deviceUniqueId`，改完之后应用未必立刻看到新值；force-stop 目标应用也不一定够，
必要时需重启 framework 或整机。**判断是否真的生效，以 `written` / `w_written` 计数为准，不要只看界面。**

### 5.3 值的呈现形式

面板与节点里是 64 位 hex；应用（如 DevCheck）显示的是 32 字节的 Base64。
换算：`echo -n <hex> | xxd -r -p | base64`。

## 6. 自检

```sh
cat /proc/susfs_drm_guard
```

| 字段 | 含义 | 期望 |
|---|---|---|
| `calls` / `read_streams` | 进入 ioctl 的次数 | 随目标应用活动增长 |
| `exact_hits` | 命中「status=0 + len=32」的回包 | 每次查询 +1 |
| `written` | 真正写入成功 | **必须 > 0** |
| `nofault_fail` | 两种写路径都失败 | 应停止增长 |
| `w_cand` / `w_written` | write_buffer 路径的候选/成功 | `w_written` 应等于成功次数 |

如果 `exact_hits` 增长而 `written` 恒为 0，先检查是不是旧版 ko（未包含 5.1 的修复）。

## 7. 回滚

```sh
# 卸载并换回旧 ko（不重启）
rmmod susfs_guard_lkm
/data/adb/modules/hideguard/susfs_insmod /path/to/old.ko expose_proc=1
```

若开机阶段崩溃：在模块目录放一个空的 `disable` 文件即可阻止开机自动加载（`rmmod` 后亦可）。

---

## 8. 重要修正（2026-10-23 晚实测）

前面 5.1 把“写入失败”全部归因于缺页，这个结论**不完整**。补充实测：

### 8.1 真正生效的是 HAL 侧 write-buffer 路径

`drm_guard` 有两条扫描路径：

| 路径 | 触发点 | 写入对象 | 实测结果 |
|---|---|---|---|
| 客户端 | `drm_ret_handler` 扫 read buffer 里的 BR_REPLY | 应用侧 parcel | ❌ `copy_to_user_nofault`\\`copy_to_user`\\`copy_to_kernel_nofault` 全部失败 |
| **HAL** | `drm_entry_handler` 中 `current_uid == haluid`，扫 write buffer 里的 BC_REPLY | **HAL 自己的 parcel** | ✅ 稳定成功（`w_cand=1 w_written=1`） |

成功时的计数形态：

```
req_seen=11  e_scan=125  e_gate=1  e_write=92
w_cand=1  w_written=1  written=1
```

### 8.2 HAL 路径有 2 秒时间窗

```c
if (!(g_apply && pend && !time_after(jiffies, pend + 2 * HZ)))
        atomic64_inc(&e_gate);   // 窗口过期 → 丢弃
```

`pend` 由请求侧匹配（`code==11` + `deviceUniqueId`）时写入。若请求没被匹配到（`req_seen=0`），
HAL 路径就永远被 gate 掉。实测把窗口放宽到 60 秒（分支 `win60`）可显著提高命中率。

### 8.3 mediadrm 缓存才是“改了不生效”的主因

`mediadrm` 会缓存 `deviceUniqueId`，**不重启 Widevine HAL 就不会重新问 HAL**，
钩子自然扫不到任何东西（表现为 `req_seen=0`、`w_cand=0`，看起来像“写入坏了”）。

正确操作顺序：

```sh
echo 'id <64hex>' > /proc/susfs_drm_guard   # 下发新值
echo 'uids <目标uid>' > /proc/susfs_drm_guard
kill $(pidof android.hardware.drm-service.widevine)   # 关键：重启 HAL
# 重开目标应用，或让它重新发起 DRM 查询
```

已验证：`kill` 后 HAL 由 init 自动拉起（pid 变化），应用再查即拿到新值。

### 8.4 结论排序

1. 改完 id 必须重启 Widevine HAL（否则一切无效）
2. 有效写入路径是 HAL write-buffer，不是客户端 read buffer
3. 客户端路径的三种写方式都不可用，不用再在这个方向尝试
