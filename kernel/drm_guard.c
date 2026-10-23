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
#include <linux/jiffies.h>
#include <linux/percpu.h>
#include <linux/random.h>
#include <linux/string.h>
#include <linux/types.h>
#include <uapi/linux/android/binder.h>

#include "drm_guard.h"

#define DRM_ID_BYTES 32
#define DRM_PARCEL_BYTES (8 + DRM_ID_BYTES)
#define DRM_SCAN_BYTES 4096
#define DRM_UID_LIMIT 32

static u8 g_vid[DRM_ID_BYTES];
static bool g_enable;
static bool g_apply;
static u32 g_uids[DRM_UID_LIMIT];
static u32 g_uid_count;
static u32 g_hal_uid;
static unsigned long g_pending; /* last DRM request (jiffies) */
static DEFINE_MUTEX(g_ctl_lock);

struct drm_call_ctx { unsigned long arg; u32 cmd; u32 reserved; };

struct drm_stats {
atomic64_t calls, read_streams, reply_seen, exact_hits;
atomic64_t would_write, written, nofault_fail;
atomic64_t c_cmd, c_ret, c_arg, c_bwr, c_noread;
atomic64_t diag;
atomic64_t stop_unknown, r_ge8, r_sz40, r_st0, r_len32;
atomic64_t diag2;
atomic64_t w_cand, w_written;
atomic64_t req_seen, w_skipped;
atomic64_t e_scan, e_gate, e_write;
atomic64_t kaddr_hits;
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

static inline unsigned long drm_untag(unsigned long a)
{
return a & 0x00FFFFFFFFFFFFFFUL;
}

static int drm_u_read(void *dst, const void __user *src, size_t n)
{
if (copy_from_user_nofault(dst, src, n)) {
if (copy_from_kernel_nofault(dst, (const void *)src, n)) {
atomic64_inc(&g_stats.nofault_fail);
return -EFAULT;
}
atomic64_inc(&g_stats.kaddr_hits);
}
return 0;
}

static int drm_u_write(void __user *dst, const void *src, size_t n)
{
if (copy_to_user_nofault(dst, src, n) && copy_to_user(dst, src, n)) {
atomic64_inc(&g_stats.nofault_fail);
return -EFAULT;
}
return 0;
}

static bool drm_try_patch(struct binder_transaction_data *td)
{
void __user *parcel = (void __user *)drm_untag((unsigned long)td->data.ptr.buffer);
u8 head[DRM_PARCEL_BYTES];
u32 status, len, i, nz = 0;

if (!parcel || td->data_size != DRM_PARCEL_BYTES || td->offsets_size)
return false;
if (drm_u_read(head, parcel, sizeof(head)))
return false;
memcpy(&status, head, 4);
memcpy(&len, head + 4, 4);
if (status == 0)
atomic64_inc(&g_stats.r_st0);
if (len == DRM_ID_BYTES)
atomic64_inc(&g_stats.r_len32);
if (atomic64_inc_return(&g_stats.diag2) <= 6)
pr_info("parcel dsz=%llu st=%u len=%u\n", (unsigned long long)td->data_size, status, len);
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
const u8 __user *stream = (const u8 __user *)drm_untag((unsigned long)bwr->read_buffer);
u32 cap, want, off = 0;
if (!stream)
return;
cap = min_t(u32, bwr->read_consumed, bwr->read_size);
want = min_t(u32, cap, DRM_SCAN_BYTES);
if (!want)
return;
atomic64_inc(&g_stats.read_streams);
while (off + sizeof(u32) <= want) {
u32 cmd, step;
struct binder_transaction_data td;
if (drm_u_read(&cmd, stream + off, sizeof(cmd))) {
if (atomic64_inc_return(&g_stats.diag) <= 3)
pr_info("scan fail tgid=%d comm=%s arg=%px rb=%px rsize=%llu rcons=%llu\n",
current->tgid, current->comm, (void *)bwr->read_buffer,
stream, (unsigned long long)bwr->read_size, (unsigned long long)bwr->read_consumed);
break;
}
step = drm_rec_size(cmd);
if (!step) {
atomic64_inc(&g_stats.stop_unknown);
break;
}
if (off + step > want)
break;
if (cmd == BR_REPLY) {
if (drm_u_read(&td, stream + off + sizeof(u32), sizeof(td)))
break;
if (td.data_size >= 8)
atomic64_inc(&g_stats.r_ge8);
if (td.data_size == 40)
atomic64_inc(&g_stats.r_sz40);
atomic64_inc(&g_stats.reply_seen);
drm_try_patch(&td);
}
off += step;
}
}

static u32 drm_bc_size(u32 cmd)
{
switch (cmd) {
case BC_TRANSACTION:
case BC_REPLY:
return sizeof(u32) + sizeof(struct binder_transaction_data);
case BC_FREE_BUFFER:
case BC_DEAD_BINDER_DONE:
return sizeof(u32) + sizeof(binder_uintptr_t);
case BC_INCREFS:
case BC_ACQUIRE:
case BC_RELEASE:
case BC_DECREFS:
case BC_ACQUIRE_RESULT:
return sizeof(u32) + sizeof(u32);
case BC_ATTEMPT_ACQUIRE:
return sizeof(u32) + 2 * sizeof(u32);
case BC_INCREFS_DONE:
case BC_ACQUIRE_DONE:
case BC_REQUEST_DEATH_NOTIFICATION:
case BC_CLEAR_DEATH_NOTIFICATION:
return sizeof(u32) + 2 * sizeof(binder_uintptr_t);
case BC_REGISTER_LOOPER:
case BC_ENTER_LOOPER:
case BC_EXIT_LOOPER:
return sizeof(u32);
default:
return 0;
}
}

static bool drm_patch_write_parcel(struct binder_transaction_data *td)
{
void __user *parcel = (void __user *)drm_untag((unsigned long)td->data.ptr.buffer);
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

atomic64_inc(&g_stats.w_cand);
if (!READ_ONCE(g_apply)) {
atomic64_inc(&g_stats.would_write);
return false;
}
if (drm_u_write((u8 __user *)parcel + 8, g_vid, DRM_ID_BYTES))
return false;
atomic64_inc(&g_stats.w_written);
atomic64_inc(&g_stats.written);
return true;
}

/*
 * 宽松匹配：有些 HAL（含 DevCheck 读 deviceUniqueId 的那条链路）会把
 * [status=0][len=32][32B id] 嵌在更长的 parcel 里，或者 data_size 不是
 * 正好 40，于是严格判定永远不命中。这里在 data_size（上限 4KB）内按字节
 * 滑动找这个 8+32 字节结构，命中后原地改写。
 * 只在 g_apply=1 且 uid 已通过 drm_uid_allowed() 时才会被调用。
 */
static bool drm_patch_write_parcel_loose(struct binder_transaction_data *td)
{
    void __user *parcel = (void __user *)drm_untag((unsigned long)td->data.ptr.buffer);
    u8 head[DRM_PARCEL_BYTES];
    u32 status, len, i, nz;
    size_t total, pos;

    if (!parcel || td->offsets_size || td->data_size < (size_t)DRM_PARCEL_BYTES)
        return false;

    total = min_t(size_t, td->data_size, (size_t)DRM_SCAN_BYTES);
    for (pos = 0; pos + (size_t)DRM_PARCEL_BYTES <= total; pos++) {
        if (drm_u_read(head, (u8 __user *)parcel + pos, sizeof(head)))
            return false;
        memcpy(&status, head, 4);
        memcpy(&len, head + 4, 4);
        if (status != 0 || len != DRM_ID_BYTES)
            continue;
        nz = 0;
        for (i = 0; i < DRM_ID_BYTES; i++)
            nz |= head[8 + i];
        if (!nz)
            continue;

        atomic64_inc(&g_stats.w_cand);
        if (!READ_ONCE(g_apply)) {
            atomic64_inc(&g_stats.would_write);
            return false;
        }
        if (drm_u_write((u8 __user *)parcel + pos + 8, g_vid, DRM_ID_BYTES))
            return false;
        atomic64_inc(&g_stats.w_written);
        atomic64_inc(&g_stats.written);
        return true;
    }
    return false;
}

static void drm_scan_write(struct binder_write_read *bwr)
{
const u8 __user *stream = (const u8 __user *)drm_untag((unsigned long)bwr->write_buffer);
u32 cap, want, off = 0;

if (!stream)
return;
cap = min_t(u32, bwr->write_consumed, bwr->write_size);
want = min_t(u32, cap, DRM_SCAN_BYTES);
if (!want)
return;
while (off + sizeof(u32) <= want) {
u32 cmd, step;
struct binder_transaction_data td;
if (drm_u_read(&cmd, stream + off, sizeof(cmd)))
break;
step = drm_bc_size(cmd);
if (!step || off + step > want)
break;
if (cmd == BC_REPLY) {
if (drm_u_read(&td, stream + off + sizeof(u32), sizeof(td)))
break;
if (!drm_patch_write_parcel(&td))
            drm_patch_write_parcel_loose(&td);
}
off += step;
}
}


static void drm_scan_request(struct binder_write_read *bwr)
{
const u8 __user *stream = (const u8 __user *)drm_untag((unsigned long)bwr->write_buffer);
u32 cap, want, off = 0;

if (!stream)
return;
cap = min_t(u32, bwr->write_consumed, bwr->write_size);
want = min_t(u32, cap, DRM_SCAN_BYTES);
if (!want)
return;
while (off + sizeof(u32) <= want) {
u32 cmd, step;
struct binder_transaction_data td;
if (drm_u_read(&cmd, stream + off, sizeof(cmd)))
break;
step = drm_bc_size(cmd);
if (!step || off + step > want)
break;
if (cmd == BC_TRANSACTION) {
if (drm_u_read(&td, stream + off + sizeof(u32), sizeof(td)))
break;
if (td.code == 11) {
WRITE_ONCE(g_pending, jiffies);
atomic64_inc(&g_stats.req_seen);
}
}
off += step;
}
}



static void drm_scan_write_entry(struct binder_write_read *bwr)
{
const u8 __user *stream = (const u8 __user *)drm_untag((unsigned long)bwr->write_buffer);
u32 cap, off = 0;

if (!stream)
return;
cap = min_t(u32, bwr->write_size, DRM_SCAN_BYTES);
if (!cap)
return;
while (off + sizeof(u32) <= cap) {
u32 cmd, step;
struct binder_transaction_data td;
if (drm_u_read(&cmd, stream + off, sizeof(cmd)))
break;
step = drm_bc_size(cmd);
if (!step || off + step > cap)
break;
if (cmd == BC_REPLY) {
if (drm_u_read(&td, stream + off + sizeof(u32), sizeof(td)))
break;
drm_patch_write_parcel(&td);
}
off += step;
}
}

static int drm_entry_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
struct drm_call_ctx *ctx = (struct drm_call_ctx *)ri->data;

if (!READ_ONCE(g_enable))
return 1;
if (!drm_uid_allowed()) {
if (READ_ONCE(g_hal_uid) && __kuid_val(current_uid()) == READ_ONCE(g_hal_uid)) {
struct binder_write_read wb;
unsigned long pend = READ_ONCE(g_pending);
atomic64_inc(&g_stats.e_scan);
if (!(READ_ONCE(g_apply) && pend && !time_after(jiffies, pend + 60 * HZ)))
atomic64_inc(&g_stats.e_gate);
else if (!drm_u_read(&wb, (void __user *)drm_untag((unsigned long)regs->regs[2]), sizeof(wb)) &&
wb.write_size && wb.write_buffer) {
atomic64_inc(&g_stats.e_write);
drm_scan_write_entry(&wb);
}
}
return 1;
}
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
if (ctx->cmd != BINDER_WRITE_READ) {
atomic64_inc(&g_stats.c_cmd);
return 0;
}
if (!ctx->arg) {
atomic64_inc(&g_stats.c_arg);
return 0;
}
if (regs_return_value(regs) < 0) {
atomic64_inc(&g_stats.c_ret);
return 0;
}
if (drm_u_read(&bwr, (void __user *)drm_untag((unsigned long)ctx->arg), sizeof(bwr))) {
atomic64_inc(&g_stats.c_bwr);
return 0;
}
if (g_hal_uid && __kuid_val(current_uid()) == g_hal_uid) {
unsigned long pend = READ_ONCE(g_pending);
if (!pend || time_after(jiffies, pend + 60 * HZ)) {
atomic64_inc(&g_stats.w_skipped);
return 0;
}
if (bwr.write_size && bwr.write_consumed)
drm_scan_write(&bwr);
return 0;
}
if (bwr.write_size && bwr.write_consumed)
drm_scan_request(&bwr);
if (!bwr.read_size || !bwr.read_consumed) {
atomic64_inc(&g_stats.c_noread);
return 0;
}
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
"binder_ioctl",
"binder_ioctl_write_read",
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
seq_printf(m, "uids=%u haluid=%u\n", READ_ONCE(g_uid_count), READ_ONCE(g_hal_uid));
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
seq_printf(m, "w_cand=%lld w_written=%lld req_seen=%lld w_skipped=%lld\n",
atomic64_read(&g_stats.w_cand), atomic64_read(&g_stats.w_written),
atomic64_read(&g_stats.req_seen), atomic64_read(&g_stats.w_skipped));
seq_printf(m, "reply ge8=%lld sz40=%lld st0=%lld len32=%lld stop_unknown=%lld\n",
atomic64_read(&g_stats.r_ge8), atomic64_read(&g_stats.r_sz40),
atomic64_read(&g_stats.r_st0), atomic64_read(&g_stats.r_len32),
atomic64_read(&g_stats.stop_unknown));
seq_printf(m, "e_scan=%lld e_gate=%lld e_write=%lld\n", atomic64_read(&g_stats.e_scan), atomic64_read(&g_stats.e_gate), atomic64_read(&g_stats.e_write));
seq_printf(m, "why cmd=%lld ret=%lld arg=%lld bwr=%lld noread=%lld\n",
   atomic64_read(&g_stats.c_cmd), atomic64_read(&g_stats.c_ret),
   atomic64_read(&g_stats.c_arg), atomic64_read(&g_stats.c_bwr),
   atomic64_read(&g_stats.c_noread));
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
} else if (!strncmp(buf, "haluid ", 7)) {
u32 v;
if (kstrtouint(buf + 7, 10, &v))
return -EINVAL;
WRITE_ONCE(g_hal_uid, v);
pr_info("hal uid set: %u\n", v);
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
