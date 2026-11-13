/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * anti_data_gate.c - tss_sdk_rcv_anti_data 通道拦截 (连坐加密抑制)
 *
 * 目标: com.tencent.tmgp.dfm 进程内 libtersafe.so 的 TssSDKOnRecvData@@TERSAFE
 * (file offset 0x1e33d4, LOAD 段 vaddr==offset 零换算).
 *
 * 实证链: R1/R2 A/B (UUID b605dbe0) 证明连坐加密 payload 经 tss_sdk_rcv_anti_data
 * 内存通道下发, 文件系统零落盘. 入口 (x0=ctx, x1=cmd_id) 为 12+ 分支常数调度树;
 * 静态逆向确认无 .text 周期 CRC 自校验 (tss_sdk_wait_verify 是服务端签名握手
 * 状态机), BRK 覆盖无暴露.
 *
 * 拦截原理 (L1 参数篡改, 6.6 uprobes 语义): handler 里改 regs->regs[1] 即改
 * cmd_id; 之后 XOL 单步流程执行原入口指令 (函数序言 stp) 并以正常控制流继续 —
 * 函数照常执行, 但调度树把被改的 cmd_id 分发到无匹配分支 (默认路径), 加密
 * 指令分支不进入. 不改 PC (pre_xol 会覆盖), 不改代码字节, 不改返回值.
 *
 * uretprobe ret_handler 记录函数返回码 (LOG 模式验证用).
 *
 * proc 接口 /proc/susfs_anti_data_gate:
 *   读: 状态 + 统计 + ring 最近 32 条 + ret ring 8 条
 *   写: mode log|skip | drop <hex32> | remap <hex32>
 *       uids N,N,N | path <so> | off <hex> | enable 0|1 | arm | disarm
 *
 * filter: UPROBE_FILTER_MMAP 时按 uid 白名单决定是否对该 mm 插断点 —
 * libtersafe.so 是腾讯全家桶通用反作弊库, 不能全局插.
 *
 * 安全: handler 上下文不许 sleep/malloc; ring 静态数组. 红线: 被识别后后果
 * 从连坐升级为主动对抗 ACE — 先小号验证.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/uaccess.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/fs.h>
#include <linux/path.h>
#include <linux/namei.h>
#include <linux/sched.h>
#include <linux/cred.h>
#include <linux/mutex.h>
#include <linux/uprobes.h>
#include <linux/version.h>
#include "anti_data_gate.h"

#define AG_PREFIX "anti_data_gate: "
#define AG_RING_N 256
#define AG_UID_N  16

/* ---- 参数 (module_param 供 arm 前预设, proc 可覆盖) ---- */
static char *g_so_path;
module_param_named(so_path, g_so_path, charp, 0644);
static unsigned long g_probe_off = 0x1e33d4;
module_param_named(probe_off, g_probe_off, ulong, 0644);

static enum { AG_LOG = 0, AG_SKIP = 1 } g_mode;
static u32 g_drop_id;    /* SKIP: 命中即改写的 cmd_id (0=关) */
static u32 g_remap_id = 0x7FFFFFFF; /* SKIP: 改写目标值 — 不匹配任何分支的哨兵 */
static u32 g_uids[AG_UID_N];
static u32 g_uid_count;
static DEFINE_MUTEX(g_ctl);
static bool g_enable;
static bool g_armed;

struct ag_ent {
    u64 ts;
    u32 cmd;
    u32 uid;
    s32 pid;
    char comm[TASK_COMM_LEN];
};
static struct ag_ent g_ring[AG_RING_N];
static u32 g_ring_head;
static atomic64_t g_n_calls, g_n_skips;

/* ---- uprobe consumers ----
 * v3: 砍掉 uretprobe (返回值记录纯属多余, 蹦床劫持返回地址是最大死机面).
 * 只保留 entry probe: 改 regs->regs[1] + ring 记录, 全部在 XOL 前完成. */
static int ag_handler(struct uprobe_consumer *self, struct pt_regs *regs)
{
    struct ag_ent *e;
    u32 cmd = (u32)regs->regs[1];
    u32 uid = __kuid_val(current_uid());
    u32 i, n, head;

    if (!READ_ONCE(g_enable))
        return 0;
    /* handler 内 uid 门控 (filter 是 mmap 时的优化, 这里是执行时硬闸) */
    n = READ_ONCE(g_uid_count);
    if (n) {
        bool hit = false;
        for (i = 0; i < n && i < AG_UID_N; i++)
            if (READ_ONCE(g_uids[i]) == uid) { hit = true; break; }
        if (!hit)
            return 0;
    }
    atomic64_inc(&g_n_calls);

    if (READ_ONCE(g_mode) == AG_SKIP && READ_ONCE(g_drop_id) &&
        cmd == READ_ONCE(g_drop_id)) {
        /* L1: 改 cmd_id 为哨兵值, 调度树将走无匹配默认路径. XOL 单步会
         * 执行原入口指令并按正常控制流继续 — 无需也不许改 PC. */
        regs->regs[1] = READ_ONCE(g_remap_id);
        atomic64_inc(&g_n_skips);
        /* 被改写的事件也记 ring (原 cmd 保留在日志) */
    }

    head = (READ_ONCE(g_ring_head) + 1) & (AG_RING_N - 1);
    WRITE_ONCE(g_ring_head, head);
    e = &g_ring[head];
    e->ts = ktime_get_ns();
    e->cmd = cmd;
    e->uid = uid;
    e->pid = task_pid_nr(current);
    memcpy(e->comm, current->comm, TASK_COMM_LEN);
    return 0;
}

/* v3: ag_ret_handler 已整体移除 — 不注册 ret_handler, uretprobe 机制不启用. */

static bool ag_filter(struct uprobe_consumer *self, enum uprobe_filter_ctx ctx,
                      struct mm_struct *mm)
{
    u32 i, n, uid;

    /* v3 修复: REGISTER 上下文不再无条件放行.
     * 原逻辑 "register/unregister 由 arm 端进程发起, 放行" 在游戏已在跑时
     * arm 会导致断点插入设备上所有已映射 libtersafe.so 的进程 (微信/QQ 等
     * 腾讯系全家桶), 爆炸半径过大且是死机嫌疑源.
     * 新逻辑: 无论何种上下文, 一律按 uid 白名单硬闸. */
    if (!READ_ONCE(g_enable))
        return false;
    n = READ_ONCE(g_uid_count);
    if (!n)
        return true;    /* 白名单空 = 观测全开 */
    uid = mm->owner ? __kuid_val(task_uid(mm->owner)) : (u32)-1;
    for (i = 0; i < n && i < AG_UID_N; i++)
        if (READ_ONCE(g_uids[i]) == uid)
            return true;
    return false;
}

static struct uprobe_consumer g_uc = {
    .handler     = ag_handler,
    /* v3: 不再注册 ret_handler — uretprobe 蹦床机制整条砍掉 */
    .filter      = ag_filter,
};

/* ---- arm/disarm ---- */
static struct path g_path;

static int ag_arm_locked(void)
{
    int ret;

    if (g_armed)
        return 0;
    if (!g_so_path || !g_so_path[0]) {
        pr_warn(AG_PREFIX "arm: so_path unset\n");
        return -ENOENT;
    }
    ret = kern_path(g_so_path, LOOKUP_FOLLOW, &g_path);
    if (ret) {
        pr_warn(AG_PREFIX "kern_path(%s)=%d\n", g_so_path, ret);
        return ret;
    }
    /* i_uid_read() 的返回类型跨内核不统一 (kuid_t/uid_t/vfsuid_t), 只做信息展示,
     * 不值得为它写兼容层 — so 属主不参与门控, 门控在 handler 的 g_uids 里. */
    ret = uprobe_register(g_path.dentry->d_inode,
                          (loff_t)READ_ONCE(g_probe_off), &g_uc);
    if (ret) {
        path_put(&g_path);
        pr_warn(AG_PREFIX "uprobe_register(%s @0x%lx)=%d\n",
                g_so_path, READ_ONCE(g_probe_off), ret);
        return ret;
    }
    g_armed = true;
    pr_info(AG_PREFIX "armed %s@0x%lx mode=%s drop=0x%x remap=0x%x\n",
            g_so_path, READ_ONCE(g_probe_off),
            READ_ONCE(g_mode) == AG_SKIP ? "skip" : "log",
            READ_ONCE(g_drop_id), READ_ONCE(g_remap_id));
    return 0;
}

static void ag_disarm_locked(void)
{
    if (!g_armed)
        return;
    uprobe_unregister(g_path.dentry->d_inode,
                      (loff_t)READ_ONCE(g_probe_off), &g_uc);
    path_put(&g_path);
    g_armed = false;
    pr_info(AG_PREFIX "disarmed\n");
}

/* ---- proc ---- */
static int ag_show(struct seq_file *m, void *v)
{
    u32 head = READ_ONCE(g_ring_head);
    u32 i, n = READ_ONCE(g_uid_count);

    seq_printf(m, "mode=%s armed=%d enable=%d\n",
               READ_ONCE(g_mode) == AG_SKIP ? "skip" : "log",
               g_armed, READ_ONCE(g_enable));
    seq_printf(m, "path=%s off=0x%lx\n",
               g_so_path ? g_so_path : "(unset)",
               READ_ONCE(g_probe_off));
    seq_printf(m, "drop=0x%x remap=0x%x\n",
               READ_ONCE(g_drop_id), READ_ONCE(g_remap_id));
    seq_printf(m, "uids=%u\n", n);
    for (i = 0; i < n && i < AG_UID_N; i++)
        seq_printf(m, "  uid[%u]=%u\n", i, READ_ONCE(g_uids[i]));
    seq_printf(m, "calls=%lld skips=%lld\n",
               atomic64_read(&g_n_calls), atomic64_read(&g_n_skips));
    seq_puts(m, "ring (newest first, last 32):\n");
    for (i = 0; i < 32; i++) {
        u32 idx = (head + AG_RING_N - 1 - i) & (AG_RING_N - 1);
        struct ag_ent *e = &g_ring[idx];
        if (!e->ts) continue;
        seq_printf(m, "  [%3u] %llu cmd=0x%-8x uid=%u pid=%d %s\n",
                   idx, e->ts, e->cmd, e->uid, e->pid, e->comm);
    }
    return 0;
}

static int ag_open(struct inode *inode, struct file *file)
{
    return single_open(file, ag_show, NULL);
}

static int ag_exec_line(const char *line)
{
    char buf[512];
    size_t len = strnlen(line, sizeof(buf) - 1);

    if (!len)
        return -EINVAL;
    memcpy(buf, line, len);
    buf[len] = 0;

    if (!strncmp(buf, "mode log", 8)) {
        WRITE_ONCE(g_mode, AG_LOG);
    } else if (!strncmp(buf, "mode skip", 9)) {
        WRITE_ONCE(g_mode, AG_SKIP);
    } else if (!strncmp(buf, "drop ", 5)) {
        u32 v;
        if (kstrtouint(buf + 5, 0, &v)) return -EINVAL;
        WRITE_ONCE(g_drop_id, v);
    } else if (!strncmp(buf, "remap ", 6)) {
        u32 v;
        if (kstrtouint(buf + 6, 0, &v)) return -EINVAL;
        WRITE_ONCE(g_remap_id, v);
    } else if (!strncmp(buf, "uids ", 5)) {
        char *p = buf + 5, *tok;
        u32 n = 0;
        while ((tok = strsep(&p, ", ")) != NULL && n < AG_UID_N) {
            u32 v;
            if (!*tok || kstrtouint(tok, 10, &v))
                continue;
            WRITE_ONCE(g_uids[n], v);
            n++;
        }
        WRITE_ONCE(g_uid_count, n);
    } else if (!strncmp(buf, "path ", 5)) {
        kfree(g_so_path);
        g_so_path = kstrdup(buf + 5, GFP_KERNEL);
        if (!g_so_path) return -ENOMEM;
    } else if (!strncmp(buf, "off ", 4)) {
        unsigned long v;
        if (kstrtoul(buf + 4, 0, &v)) return -EINVAL;
        WRITE_ONCE(g_probe_off, v);
    } else if (!strncmp(buf, "enable ", 7)) {
        u32 v;
        if (kstrtouint(buf + 7, 0, &v)) return -EINVAL;
        WRITE_ONCE(g_enable, v ? 1 : 0);
    } else if (!strncmp(buf, "arm", 3) &&
               (buf[3] == 0 || buf[3] == '\n' || buf[3] == ' ')) {
        return ag_arm_locked();
    } else if (!strncmp(buf, "disarm", 6)) {
        ag_disarm_locked();
    } else {
        return -EINVAL;
    }
    return 0;
}

static ssize_t ag_write(struct file *file, const char __user *ubuf,
                        size_t count, loff_t *ppos)
{
    char buf[512];
    size_t len = min_t(size_t, count, sizeof(buf) - 1);
    int ret;

    if (copy_from_user(buf, ubuf, len))
        return -EFAULT;
    buf[len] = 0;
    while (len && (buf[len-1] == '\n' || buf[len-1] == '\r'))
        buf[--len] = 0;

    mutex_lock(&g_ctl);
    ret = ag_exec_line(buf);
    mutex_unlock(&g_ctl);
    return ret ? ret : count;
}

static const struct proc_ops ag_proc_ops = {
    .proc_open    = ag_open,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_write   = ag_write,
    .proc_release = single_release,
};

static struct proc_dir_entry *g_proc;

/* ---- init/exit ---- */
/* 模块加载时自动读取 /sdcard/Download/hg_antidata.conf：逐行按 proc 相同语法执行。
 * 目的：全链路自动化，无需任何手动 root 命令（proc 节点被 sus_path 隐藏层保护，
 * 无 root 无法访问；Root/Shizuku 通道不可靠）。文件不存在 = 保持默认 LOG 不 arm。 */
static void ag_boot_conf(void)
{
    struct file *f;
    char buf[512];
    char carry[512];
    size_t carry_len = 0;
    loff_t pos = 0;
    int n;

    mutex_lock(&g_ctl);
    f = filp_open("/sdcard/Download/hg_antidata.conf", O_RDONLY, 0);
    if (IS_ERR(f)) {
        mutex_unlock(&g_ctl);
        pr_info(AG_PREFIX "no boot conf, staying LOG\n");
        return;
    }
    while ((n = kernel_read(f, buf, sizeof(buf) - 1, &pos)) > 0) {
        char *line, *p;
        size_t keep = 0;
        buf[n] = 0;
        /* 拼接上一轮未完成的半行 */
        if (carry_len) {
            if (carry_len + n >= sizeof(buf)) carry_len = 0; /* 太长丢弃 */
            else { memmove(buf + carry_len, buf, n + 1); memcpy(buf, carry, carry_len); }
        }
        p = buf;
        while ((line = strsep(&p, "\n")) != NULL) {
            if (p == NULL && strlen(line)) {   /* 末行未带换行: 可能跨读, 存 carry */
                strncpy(carry, line, sizeof(carry) - 1);
                carry[sizeof(carry) - 1] = 0;
                keep = strlen(carry);
                continue;
            }
            {
                size_t ll = strlen(line);
                while (ll && (line[ll-1] == '\r' || line[ll-1] == ' ')) line[--ll] = 0;
                if (!ll) continue;
                ag_exec_line(line);
            }
        }
        carry_len = keep;
        (void)carry_len;
    }
    /* 文件末行(无换行)也执行 */
    if (carry_len) {
        carry[carry_len] = 0;
        ag_exec_line(carry);
    }
    filp_close(f, NULL);
    mutex_unlock(&g_ctl);
    pr_info(AG_PREFIX "boot conf applied\n");
}

int anti_data_gate_init(void)
{
    g_proc = proc_create("susfs_anti_data_gate", 0644, NULL, &ag_proc_ops);
    if (!g_proc)
        return -ENOMEM;
    ag_boot_conf();
    pr_info(AG_PREFIX "init: LOG 模式 (安全观测)\n");
    return 0;
}

void anti_data_gate_exit(void)
{
    mutex_lock(&g_ctl);
    ag_disarm_locked();
    mutex_unlock(&g_ctl);
    if (g_proc)
        proc_remove(g_proc);
    kfree(g_so_path);
    pr_info(AG_PREFIX "exit\n");
}
/* ---- 独立模块入口：不再依赖 susfs_main 调用，可单独 insmod/rmmod ---- */
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("anti_data_gate solo LKM - tss_sdk_rcv_anti_data channel gate");
MODULE_AUTHOR("yg");

static int __init ag_solo_init(void)
{
    int ret = anti_data_gate_init();
    if (ret) {
        pr_warn(AG_PREFIX "solo init failed: %d\n", ret);
        return ret;
    }
    pr_info(AG_PREFIX "solo module loaded (independent, coexists with susfs_guard_lkm)\n");
    return 0;
}

static void __exit ag_solo_exit(void)
{
    anti_data_gate_exit();
    pr_info(AG_PREFIX "solo module unloaded\n");
}

module_init(ag_solo_init);
module_exit(ag_solo_exit);
