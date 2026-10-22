/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * drm_guard.c - Widevine "deviceUniqueId" interception, M2 (per-uid, opt-in).
 *
 * 为什么 M1/M1.5 会把系统卡死：kprobe handler 跑在关抢占的原子上下文，
 * copy_from_user() 在页不存在时会走缺页流程、可能睡眠 -> scheduling while
 * atomic，而 binder_ioctl 是每秒上万次的热点，于是 binder 线程池被钉死。
 * 这里用户内存一律 copy_*_user_nofault()（内部 pagefault_disable，不睡眠），
 * 缓冲区改用 per-CPU 静态块，不再 kmalloc(GFP_ATOMIC)。
 *
 * 触发模型（M1.5 实测）：请求和回复经常不在同一次 ioctl；BR_REPLY 的 code
 * 恒为 0、sender_pid 恒为 0，回复无法自证身份。因此只能靠「40 字节的
 * [0][32][32B] 回复 + uid 白名单」匹配，并默认 dry-run / fail-closed。
 */
#define pr_fmt(fmt) "drm_guard: " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/mutex.h>
#include <linux/atomic.h>
#include <linux/percpu.h>
#include <linux/random.h>
#include <linux/string.h>
#include <linux/types.h>
#include <uapi/linux/android/binder.h>

#include "drm_guard.h"

#define DRM_ID_BYTES 32
#define DRM_PARCEL_BYTES (8 + DRM_ID_BYTES)
#define DRM_SCAN_BYTES 512
#define DRM_UID_LIMIT 32

static u8 g_vid[DRM_ID_BYTES];
static bool g_enable;
static bool g_apply;
static u32 g_uids[DRM_UID_LIMIT];
static u32 g_uid_count;
static DEFINE_MUTEX(g_ctl_lock);

struct drm_call_ctx { unsigned long arg; u32 cmd; u32 reserved; };

struct drm_stats {
atomic64_t calls, read_streams, reply_seen, exact_hits;
atomic64_t would_write, written, nofault_fail;
};

static struct drm_stats g_stats;

static bool drm_uid_allowed(void)
{
u32 uid = __kuid_val(current_uid());
u32 i, n = READ_ONCE(g_uid_count);

if (!n)
return false;
for (i = 0; i < n && i < DRM_UID_LIMIT; i++)
if (READ_ONCE(g_uids[i]) == uid)
return true;
return false;
}

static int drm_u_read(void *dst, const void __user *src, size_t n)
{
if (copy_from_user_nofault(dst, src, n)) {
atomic64_inc(&g_stats.nofault_fail);
return -EFAULT;
}
return 0;
}

static int drm_u_write(void __user *dst, const void *src, size_t n)
{
if (copy_to_user_nofault(dst, src, n)) {
atomic64_inc(&g_stats.nofault_fail);
return -EFAULT;
}
return 0;
}

static bool drm_try_patch(struct binder_transaction_data *td)
{
void __user *parcel = (void __user *)td->data.ptr.buffer;
u8 head[DRM_PARCEL_BYTES];
u32 status, len, i, nz = 0;

if (!parcel || td->data_size != DRM_PARCEL_BYTES || td->offsets_size)
return false;
if (drm_u_read(head, parcel, sizeof(head)))
return false;
memcpy(&status, head, 4);
memcpy(&len, head + 4, 4);
if (status != 0 || len != DRM_ID_BYTES)
return false;
for (i = 0; i < DRM_ID_BYTES; i++)
nz |= head[8 + i];
if (!nz)
return false;

atomic64_inc(&g_stats.exact_hits);
if (!READ_ONCE(g_apply)) {
atomic64_inc(&g_stats.would_write);
return false;
}
if (drm_u_write((u8 __user *)parcel + 8, g_vid, DRM_ID_BYTES))
return false;
atomic64_inc(&g_stats.written);
return true;
}

static u32 drm_rec_size(u32 cmd)
{
switch (cmd) {
case BR_TRANSACTION:
case BR_REPLY:
return sizeof(u32) + sizeof(struct binder_transaction_data);
case BR_INCREFS:
case BR_ACQUIRE:
case BR_RELEASE:
case BR_DECREFS:
case BR_ATTEMPT_ACQUIRE:
return sizeof(u32) + 2 * sizeof(binder_uintptr_t);
case BR_DEAD_BINDER:
case BR_CLEAR_DEATH_NOTIFICATION_DONE:
return sizeof(u32) + sizeof(binder_uintptr_t);
case BR_NOOP:
case BR_TRANSACTION_COMPLETE:
case BR_DEAD_REPLY:
case BR_FAILED_REPLY:
case BR_SPAWN_LOOPER:
return sizeof(u32);
default:
return 0;
}
}

static void drm_scan_read(struct binder_write_read *bwr)
{
const u8 __user *stream = (const u8 __user *)bwr->read_buffer;
u32 want = min_t(u32, bwr->read_consumed, DRM_SCAN_BYTES);
u32 off = 0;
if (!stream || !want)
return;
atomic64_inc(&g_stats.read_streams);
while (off + sizeof(u32) <= want) {
u32 cmd, step;
struct binder_transaction_data td;
if (drm_u_read(&cmd, stream + off, sizeof(cmd)))
return;
step = drm_rec_size(cmd);
if (!step || off + step > want)
return;
if (cmd == BR_REPLY) {
if (drm_u_read(&td, stream + off + sizeof(u32), sizeof(td)))
return;
atomic64_inc(&g_stats.reply_seen);
drm_try_patch(&td);
}
off += step;
}
}

static int drm_entry_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
struct drm_call_ctx *ctx = (struct drm_call_ctx *)ri->data;

if (!READ_ONCE(g_enable))
return 1;
if (!drm_uid_allowed())
return 1;
ctx->arg = regs->regs[2];
ctx->cmd = (u32)regs->regs[1];
ctx->reserved = 0;
atomic64_inc(&g_stats.calls);
return 0;
}

static int drm_ret_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
struct drm_call_ctx *ctx = (struct drm_call_ctx *)ri->data;
struct binder_write_read bwr;

if (ctx->cmd != BINDER_WRITE_READ || !ctx->arg)
return 0;
if (regs_return_value(regs) < 0)
return 0;
if (drm_u_read(&bwr, (void __user *)ctx->arg, sizeof(bwr)))
return 0;
if (!bwr.read_size || !bwr.read_consumed)
return 0;
drm_scan_read(&bwr);
return 0;
}

static struct kretprobe g_krp = {
.entry_handler = drm_entry_handler,
.handler = drm_ret_handler,
.data_size = sizeof(struct drm_call_ctx),
.maxactive = 32,
};

static const char *const g_probe_symbols[] = {
"binder_ioctl_write_read",
"binder_ioctl",
};

static bool g_armed;

static int drm_arm_locked(void)
{
size_t i;
int ret = -ENOENT;

if (g_armed)
return 0;
for (i = 0; i < ARRAY_SIZE(g_probe_symbols); i++) {
g_krp.kp.symbol_name = g_probe_symbols[i];
g_krp.kp.addr = NULL;
ret = register_kretprobe(&g_krp);
if (!ret) {
g_armed = true;
pr_info("kretprobe armed on %s\n", g_krp.kp.symbol_name);
return 0;
}
g_krp.kp.addr = NULL;
}
pr_warn("no probeable binder entry (%d)\n", ret);
return ret;
}

static void drm_disarm_locked(void)
{
if (!g_armed)
return;
unregister_kretprobe(&g_krp);
g_krp.kp.addr = NULL;
g_armed = false;
pr_info("kretprobe disarmed\n");
}

static int drm_proc_show(struct seq_file *m, void *v)
{
u32 i;

seq_printf(m, "mode=%s\n", READ_ONCE(g_apply) ? "apply" : "dry-run");
seq_printf(m, "enabled=%d armed=%d probe=%s\n", READ_ONCE(g_enable), g_armed,
   g_krp.kp.symbol_name ? g_krp.kp.symbol_name : "off");
seq_printf(m, "uids=%u\n", READ_ONCE(g_uid_count));
for (i = 0; i < READ_ONCE(g_uid_count) && i < DRM_UID_LIMIT; i++)
seq_printf(m, "  uid[%u]=%u\n", i, READ_ONCE(g_uids[i]));
seq_puts(m, "id=");
for (i = 0; i < DRM_ID_BYTES; i++)
seq_printf(m, "%02x", g_vid[i]);
seq_putc(m, '\n');
seq_printf(m, "calls=%lld\n", atomic64_read(&g_stats.calls));
seq_printf(m, "read_streams=%lld\n", atomic64_read(&g_stats.read_streams));
seq_printf(m, "reply_seen=%lld\n", atomic64_read(&g_stats.reply_seen));
seq_printf(m, "exact_hits=%lld\n", atomic64_read(&g_stats.exact_hits));
seq_printf(m, "would_write=%lld\n", atomic64_read(&g_stats.would_write));
seq_printf(m, "written=%lld\n", atomic64_read(&g_stats.written));
seq_printf(m, "nofault_fail=%lld\n", atomic64_read(&g_stats.nofault_fail));
return 0;
}

static int drm_proc_open(struct inode *inode, struct file *file)
{
return single_open(file, drm_proc_show, NULL);
}

static int drm_hex2bin(const char *s, u8 *out, int len)
{
int i, k;

for (i = 0; i < len; i++) {
u8 v = 0;
for (k = 0; k < 2; k++) {
char c = s[i * 2 + k];
v <<= 4;
if (c >= '0' && c <= '9')
v |= c - '0';
else if (c >= 'a' && c <= 'f')
v |= c - 'a' + 10;
else if (c >= 'A' && c <= 'F')
v |= c - 'A' + 10;
else
return -EINVAL;
}
out[i] = v;
}
return 0;
}

static ssize_t drm_proc_write(struct file *file, const char __user *ubuf,
      size_t count, loff_t *ppos)
{
char buf[256];
size_t len = min_t(size_t, count, sizeof(buf) - 1);
int ret = count;

if (copy_from_user(buf, ubuf, len))
return -EFAULT;
buf[len] = 0;
while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
buf[--len] = 0;

if (!strncmp(buf, "uids ", 5)) {
char *p = buf + 5, *tok;
u32 n = 0;
mutex_lock(&g_ctl_lock);
while ((tok = strsep(&p, ", ")) != NULL && n < DRM_UID_LIMIT) {
u32 v;
if (!*tok || kstrtouint(tok, 10, &v))
continue;
WRITE_ONCE(g_uids[n], v);
n++;
}
WRITE_ONCE(g_uid_count, n);
mutex_unlock(&g_ctl_lock);
pr_info("uids published: %u\n", n);
} else if (!strncmp(buf, "id ", 3)) {
u8 tmp[DRM_ID_BYTES];
if (drm_hex2bin(buf + 3, tmp, DRM_ID_BYTES))
return -EINVAL;
mutex_lock(&g_ctl_lock);
memcpy(g_vid, tmp, DRM_ID_BYTES);
mutex_unlock(&g_ctl_lock);
pr_info("virtual id set\n");
} else if (!strcmp(buf, "apply 1")) {
WRITE_ONCE(g_apply, true);
pr_warn("apply mode ON: replies will be rewritten\n");
} else if (!strcmp(buf, "apply 0")) {
WRITE_ONCE(g_apply, false);
pr_info("dry-run mode\n");
} else if (!strcmp(buf, "enable 1")) {
mutex_lock(&g_ctl_lock);
WRITE_ONCE(g_enable, true);
if (drm_arm_locked())
WRITE_ONCE(g_enable, false);
mutex_unlock(&g_ctl_lock);
} else if (!strcmp(buf, "enable 0")) {
mutex_lock(&g_ctl_lock);
WRITE_ONCE(g_enable, false);
drm_disarm_locked();
mutex_unlock(&g_ctl_lock);
} else if (!strcmp(buf, "reset")) {
atomic64_set(&g_stats.calls, 0);
atomic64_set(&g_stats.read_streams, 0);
atomic64_set(&g_stats.reply_seen, 0);
atomic64_set(&g_stats.exact_hits, 0);
atomic64_set(&g_stats.would_write, 0);
atomic64_set(&g_stats.written, 0);
atomic64_set(&g_stats.nofault_fail, 0);
} else {
return -EINVAL;
}
return ret;
}

static const struct proc_ops drm_proc_ops = {
.proc_open = drm_proc_open,
.proc_read = seq_read,
.proc_lseek = seq_lseek,
.proc_release = single_release,
.proc_write = drm_proc_write,
};

static struct proc_dir_entry *g_proc;

int drm_guard_init(void)
{
get_random_bytes(g_vid, DRM_ID_BYTES);
g_enable = false;
g_apply = false;
g_uid_count = 0;
g_proc = proc_create("susfs_drm_guard", 0644, NULL, &drm_proc_ops);
if (!g_proc)
pr_warn("proc entry creation failed\n");
pr_info("ready: disabled, dry-run, no uid whitelist\n");
return 0;
}

void drm_guard_exit(void)
{
mutex_lock(&g_ctl_lock);
WRITE_ONCE(g_enable, false);
drm_disarm_locked();
mutex_unlock(&g_ctl_lock);
if (g_proc) {
proc_remove(g_proc);
g_proc = NULL;
}
}
