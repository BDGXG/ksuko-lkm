# aidcopy 探针 v7（读内核侧 binder buffer）— WIP 备份

日期：2026-10-28/29　状态：**未完成，未上线**　样本/目标：用户自有真机 OnePlus 13（KernelSU，6.6.118-android15-8-gok）

## 一、v6 结论（v7 的动因）
- v6 用 `ac_copy_in()`（4K 分页 `copy_from_user_nofault`，失败页清零）替换整块拷贝。
- 实测：`ac_dst_seen=3` 恰等于冷启动次数，`ac_hdump/ac_dump` 为空，`ac_bad` 占 53%，`ac_scan` 63→298（只涨了 client→server 侧）。
- **架构结论**：reply 方向的 `from` 属 system_server 的 mm，`copy_from_user_nofault` 在非属主 mm 下必然 EFAULT，分页也救不回 → 靠读用户态源地址这条路在 reply 方向走不通。

## 二、v7 设计
- 换轨：不再读用户态 `from`，改读**内核侧** binder 缓冲。
- kprobe `binder_alloc_copy_user_to_buffer`（client→server，存量，v5/v6 反向判定）保留。
- 新增 kretprobe `binder_alloc_copy_from_buffer(alloc, dest, buffer_off, bytes)`：
  - entry：`regs[1]=dest`（内核目标缓冲）、`regs[3]=bytes`；`bytes<32` 跳过；白名单判定用 **`current_uid()`**（此时 current 就是收 reply 的 app 进程，与 v5 用 `alloc->pid` 相反，这是本方案的关键简化）。
  - return：扫描 `dest` 内容，找 `[int32 len=16][16 位 hex]`，UTF-16LE（hi 字节必须 0）与 ASCII 双模式；命中回填 offset，并把前 `AC_DUMP_BYTES` 拷进 `g_ac_dump`。
  - `maxactive=32` 防并发溢出；`data_size=sizeof(struct kb_req)`。
- 全程 dry-run 只读，不写任何用户/内核内存。

## 三、v7 代码改动清单（已落地于 kernel/drm_guard.c）
1. `struct kb_req { void *dest; u32 bytes; };`、`static struct kretprobe g_kb_rp;`
2. `static atomic64_t g_kb_calls, g_kb_scan, g_kb_hit, g_kb_off;`
3. `kb_find_aid()`（`__nocfi noinline`）、`kb_entry_handler()`、`kb_ret_handler()`（`__nocfi`）
4. `ac_arm_locked()`：kprobe 注册成功后 `register_kretprobe(&g_kb_rp)`，失败 `g_ac_nosym++` + `pr_warn`
5. `ac_disarm_locked()`：条件 `unregister_kretprobe(&g_kb_rp)` 并清 `addr`/`symbol_name`
6. proc 读侧：`kb calls=%lld scan=%lld hit=%lld off=%lld`
7. reset：`g_kb_calls/g_kb_scan/g_kb_hit/g_kb_off` 四项清零

## 四、构建记录
- 第一轮 run `40688440179` **failure**，唯一错误：
  `drm_guard.c:625: error: assigning to 'volatile typeof (g_kb_off)' (aka 'volatile atomic64_t') from incompatible type 'u32'`
  —— `WRITE_ONCE(g_kb_off, off)` 对 `atomic64_t` 不合法。
- 修复：`WRITE_ONCE` → `atomic64_set(&g_kb_off, off);`（proc 读侧已用 `atomic64_read`、reset 已用 `atomic64_set`，一致）。
- 修复后第二轮 run `40689114263`，拉取备份时仍 in_progress（结果未取回）。
- 构建命令（token 必须与 export 同一命令行，长度 40）：
  ```
  cd /root; export GH_TOKEN=$(grep -o "ghp_[A-Za-z0-9]*" push_v196.py | head -1); \
  cd work/cur; cp ../../hg/src/kernel/drm_guard.c drm_guard_v2.c; \
  nohup python3 /root/c_build_ac.py > buildN.log 2>&1 &
  ```

## 五、产物基线（防误把旧 ko 当新品上线）
| 版本 | 大小 | sha256 |
| --- | --- | --- |
| v3 | 2702536 | fdbf354b… |
| v4 | 2702376 | 08ad975d… |
| v5 | 2710632 | f08f0d10… |
| v6 | 2712504 | 43c39b64…（run 40687533279，最后一个已验证可用版） |
| v7 | 待构建确认 | ≠ 上表 |

## 六、下一步（明天续）
1. 取 run `40689114263` 结论；若 fail 再拉 job 日志（`/actions/jobs/{id}/logs` 会 302 到 blob，须用自定义 `HTTPRedirectHandler` 在 redirect 时 pop 掉 `Authorization` 头，否则 403）。
2. 成功则核对 mtime + 大小 + sha256，然后热替换：`cat /sys/module/susfs_guard_lkm/refcnt`（0）→ `rmmod` → `susfs_insmod ./ko/6.6.ko expose_proc=1`（期望 `resolved 112/112; unresolved 0` + OK）。
3. 验证：
   ```
   echo 'uids 10359 10420 10452 10435' > /proc/susfs_drm_guard
   echo 'aidcopy 1' > /proc/susfs_drm_guard
   echo reset > /proc/susfs_drm_guard
   am force-stop com.liuzh.deviceinfo
   am start -W -n com.liuzh.deviceinfo/.splash.SplashActivity
   grep -E 'ac_|kb ' /proc/susfs_drm_guard
   ```
   关注 `kb calls` 是否增长（kretprobe 命中）、`kb hit` 是否 >0、`kb off` 与 `ac_dump` 首部是否出现 `[int32 16][16 hex]`。

## 七、回退
- 源码回退点：`/root/work/cur/drm_guard_v4_backup.c`（旧）、git `6961887`（v7 之前本地 HEAD）。
- 真机回退：`rmmod susfs_guard_lkm` 后加载 v6 ko（`43c39b64…`，2712504 B）。
