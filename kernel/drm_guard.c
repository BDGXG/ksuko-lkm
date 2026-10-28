/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * drm_guard.c - Widevine "deviceUniqueId" interception, M2 (per-uid, opt-in).
 *
 * 为什么 M1/M1.5 会把系统卡死：kprobe handler 跑在关抢占的原子上下文，
 * copy_from_user() 在页不存在时会走缺页流程、可能睡眠 -> scheduling while
 * atomic，而 binder_ioctl 是每秒上万次的热点，于是 binder 线程池被钉死。
 * 这里用户内存一律 copy_*_user_nofault()（内部 pagefault_disable，不睡眠），
 * 缓冲区改用模块静态块 + spinlock，不再 kmalloc(GFP_ATOMIC)。
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
#include <linux/sched/task.h>
#include <linux/uaccess.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/jiffies.h>
#include <linux/percpu.h>
#include <linux/random.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/cred.h>
#include <linux/pid.h>
#include <uapi/linux/android/binder.h>

#include "drm_guard.h"
#include "btf_layout.h"
#include "symbol_resolver.h"
#include "ksu_allow_gate.h"	/* the control-node gate is su's own */

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

/* 导出给 taint_spoof：同一份 target uid 白名单，避免维护两套配置。
 * 只读 g_uid_count / g_uids，无锁；写入侧用 WRITE_ONCE + mutex，
 * 读到半更新的列表最多是「多放行一个 uid」，不会越界。 */
bool drm_uid_allowed(void)
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

u32 drm_uid_count(void)
{
return READ_ONCE(g_uid_count);
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
if (copy_to_user_nofault(dst, src, n)) {
if (copy_to_user(dst, src, n)) {
if (copy_to_kernel_nofault((void *)dst, src, n)) {
atomic64_inc(&g_stats.nofault_fail);
return -EFAULT;
}
atomic64_inc(&g_stats.kaddr_hits);
}
}
return 0;
}

/* =====================================================================
 * android_id guard
 *
 * 目标：拦截 ContentResolver.call("GET_secure","android_id") 的 binder
 * 回复，把 Bundle 里的 16 位 hex 值原地替换成配置的假值。
 *
 * 为什么走 binder reply 而不是 SettingsProvider 存储：
 *   - 改 /data/system/users/0/settings_secure.xml 需要重启 system_server
 *     或刷新 SettingsProvider 缓存，风险高（实测会死机）。
 *   - Android 8+ 的 android_id 是 per-app 派生值（HMAC(真值, 签名)），
 *     存储真值改了也不等于目标 app 读到新值。
 *   - 游戏读 android_id 的真实路径是 ContentResolver.call -> binder ->
 *     system_server，回复 parcel 里就是派生后的 16 位 hex，直接改这里
 *     最贴近"目标 app 看到什么"。
 *
 * 匹配策略（宽松，不依赖 Bundle 精确布局）：
 *   1. 在 reply parcel 里搜 UTF-16LE 的 "android_id"（10 字符 = 20 字节）
 *   2. 从 key 之后向后最多 256 字节，找 [len=16][16 个 UTF-16 hex]
 *   3. 命中即原地改写这 16 个字符
 *   只对 uid 白名单（复用 drm 的 target_uids）生效，默认 dry-run。
 * ===================================================================== */
#define AID_LEN 16
#define AID_SCAN_BYTES 4096
static char g_aid[AID_LEN + 1];
static bool g_aid_enable;
static bool g_aid_apply;
static atomic64_t g_aid_scan, g_aid_key_hits, g_aid_hits, g_aid_written;
/* 扫描缓冲：模块 .bss 里一块 4KB 静态区，用 spinlock 串行化。
 * 不能用 DEFINE_PER_CPU —— 模块 per-cpu 预留区只有 8KB，
 * 4KB/CPU x 8 CPU 会在 load_module 阶段直接 ENOMEM（实测 rc=139）。
 * kretprobe 上下文不能睡眠，抢不到锁就跳过这一包。 */
static u8 aid_buf[AID_SCAN_BYTES];
static DEFINE_SPINLOCK(aid_lock);
/* SettingsProvider GET_secure 的回复 Bundle 里 key 是 "value"（AOSP
 * Settings.NameValueTable.VALUE）；"android_id" 是请求参数，通常只在
 * 请求 parcel 里。两个都试，谁命中算谁。 */
static const u8 aid_key_value[] = {
	'v', 0, 'a', 0, 'l', 0, 'u', 0, 'e', 0,
};
static const u8 aid_key_androidid[] = {
	'a', 0, 'n', 0, 'd', 0, 'r', 0, 'o', 0,
	'i', 0, 'd', 0, '_', 0, 'i', 0, 'd', 0,
};
struct aid_key { const u8 *p; u32 chars; };
static const struct aid_key aid_keys[] = {
	{ aid_key_value, 5 },
	{ aid_key_androidid, 10 },
};
static inline bool aid_is_hex(u8 c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
	       (c >= 'A' && c <= 'F');
}
static bool aid_try_patch(struct binder_transaction_data *td)
{
	void __user *parcel = (void __user *)drm_untag((unsigned long)td->data.ptr.buffer);
	u8 *buf;
	size_t total, pos, q, i;
	u32 k, klen;
	bool ret = false;

	if (!parcel || !READ_ONCE(g_aid_enable))
		return false;
	if (td->data_size < 64)
		return false;
	total = min_t(size_t, td->data_size, (size_t)AID_SCAN_BYTES);
	if (!spin_trylock(&aid_lock))
		return false;
	buf = aid_buf;
	if (drm_u_read(buf, parcel, total))
		goto out;
	atomic64_inc(&g_aid_scan);

	for (k = 0; k < ARRAY_SIZE(aid_keys); k++) {
		klen = aid_keys[k].chars * 2;
		for (pos = 0; pos + klen <= total; pos++) {
			if (memcmp(buf + pos, aid_keys[k].p, klen))
				continue;
			atomic64_inc(&g_aid_key_hits);
			for (q = pos + klen;
			     q + 4 + AID_LEN * 2 <= total && q < pos + 256; q++) {
				u32 len;

				memcpy(&len, buf + q, 4);
				if (len != AID_LEN)
					continue;
				for (i = 0; i < AID_LEN; i++) {
					u8 lo = buf[q + 4 + i * 2];
					u8 hi = buf[q + 4 + i * 2 + 1];

					if (hi != 0 || !aid_is_hex(lo))
						break;
				}
				if (i != AID_LEN)
					continue;
				atomic64_inc(&g_aid_hits);
				if (!READ_ONCE(g_aid_apply))
					goto out;
				{
					u8 fake[AID_LEN * 2];

					for (i = 0; i < AID_LEN; i++) {
						fake[i * 2] = (u8)g_aid[i];
						fake[i * 2 + 1] = 0;
					}
					if (drm_u_write((u8 __user *)parcel + q + 4,
							fake, AID_LEN * 2))
						goto out;
					atomic64_inc(&g_aid_written);
				}
				ret = true;
				goto out;
			}
		}
	}
out:
	spin_unlock(&aid_lock);
	return ret;
}

/* =====================================================================
 * aidcopy probe (M3, dry-run only)
 *
 * 旧 aid 段改的是 binder_ioctl 返回后 td->data.ptr.buffer 指向的用户态
 * vma（PROT_READ），drm_u_write 必然 EFAULT：真机上 aid_written 恒 0。
 * 这一版只观察 parcel 真正的落点——binder_transaction() 里唯一装载
 * parcel 明文的调用 binder_alloc_copy_user_to_buffer(alloc, buffer,
 * buffer_offset, from, bytes)，入口的 from/bytes 就是发送方 parcel 的
 * 起始地址与长度。全程只读：copy_from_user_nofault 拷进模块静态缓冲
 * 扫 UTF-16 key，一个字节都不写。命中即证明目标进程的 parcel 里确实
 * 携带 16 位 hex 回值，为后续在 buffer 内核地址上改写铺路。
 *
 * 目标进程判定：alloc 属于接收方（target_proc），alloc->pid 反查
 * task->cred->uid；命中 g_uids 白名单单独计数（uid_hit）。
 * 默认关闭，proc 里 "aidcopy 1" 才 arm。
 * ===================================================================== */
#define AC_SCAN_BYTES 16384
static bool g_ac_enable;
static u8 ac_buf[AC_SCAN_BYTES];
static DEFINE_SPINLOCK(ac_lock);
static atomic64_t g_ac_calls, g_ac_scan, g_ac_key, g_ac_hit, g_ac_uid_hit;
static atomic64_t g_ac_big, g_ac_bad, g_ac_nosym;
static int g_ac_last_pid = -1;
static u32 g_ac_last_uid;
static u32 g_ac_last_bytes;
static int g_ac_last_key = -1;	/* -1 none, 0 "value", 1 "android_id" */
static u32 g_ac_last_keyoff;	/* 首个 key 命中在 parcel 内的偏移 */
/* key 命中处的原始字节快照（hex 打印）：值可能是 UTF-16 也可能是
 * ASCII byte[]，先把现场留证，再决定匹配规则。 */
#define AC_DUMP_BYTES 512
static u8 g_ac_dump[AC_DUMP_BYTES];
static u32 g_ac_dump_len;
/* 16 位 hex 命中处的现场快照（从长度字段前 4 字节开始），单独一份，
 * 避免被 key 快照覆盖。 */
#define AC_HDUMP_BYTES 64
static u8 g_ac_hdump[AC_HDUMP_BYTES];
static u32 g_ac_hdump_len;
static u32 g_ac_hdump_off;
/* 反向（reply）方向：这个 hook 同时也承担 server->client 的数据装载。
 * 那种调用里 alloc 属于接收方（目标 app），alloc->pid 反查出的 uid
 * 就是目标进程；current 则是发送方（如 system_server, uid 1000）。
 * 因此 must 用 alloc->pid 判定 uid，才能在回复 parcel 里抓到 aid。 */
static atomic64_t g_ac_dst_scan, g_ac_dst_hit;
static atomic64_t g_ac_dst_seen;
static int g_ac_last_dstpid = -1;
static u32 g_ac_last_dstuid;
static size_t g_ac_pid_off = (size_t)-1;
static size_t g_ac_cred_uid_off = 8;
static bool g_ac_cred_uid_ok;
static struct kprobe g_ac_kp;
static bool g_ac_armed;
static struct task_struct *(*ac_find_task_fn)(pid_t);
static const struct cred *(*ac_task_cred_fn)(struct task_struct *);

/* noinline __nocfi: 两次间接调用（find_get_task_by_vpid / get_task_cred 都是
 * kallsyms 解析出来的地址）。CFI 检查的是 call site，模块里没有对应的类型
 * 信息，真机上会在 ldur 处 panic —— 与 ksu_allow_gate.c 同一处理。 */
static u32 __nocfi noinline ac_pid_to_uid(int pid)
{
	struct task_struct *tsk;
	const struct cred *cred;
	u32 uid = 0;

	if (!ac_find_task_fn || !ac_task_cred_fn || !g_ac_cred_uid_ok || pid <= 0)
		return 0;
	tsk = ac_find_task_fn(pid);
	if (!tsk)
		return 0;
	cred = ac_task_cred_fn(tsk);
	if (cred) {
		memcpy(&uid, (const char *)cred + g_ac_cred_uid_off, sizeof(uid));
		put_cred(cred);
	}
	put_task_struct(tsk);
	return uid;
}

static bool ac_uid_listed(u32 uid)
{
	u32 i, n = READ_ONCE(g_uid_count);
	for (i = 0; i < n && i < DRM_UID_LIMIT; i++)
		if (READ_ONCE(g_uids[i]) == uid)
return true;
	return false;
}
/* 命中记录：调用者必须已持有 ac_lock，本函数不释放锁，也不改返回流程。
 * k = -1 表示命中的不是 aid_keys[] 字符串，而是白名单 uid 的 parcel
 * 里直接出现的 16 位 hex 值（Bundle String，UTF-16LE 或 ASCII）。 */
static void ac_rec_hit(void *alloc, u32 bytes, u32 q, int k)
{
	int pid = -1;
	u32 cuid = __kuid_val(current_uid());

	if (alloc && g_ac_pid_off != (size_t)-1)
		memcpy(&pid, (char *)alloc + g_ac_pid_off, sizeof(pid));
	atomic64_inc(&g_ac_hit);
	WRITE_ONCE(g_ac_last_pid, pid);
	WRITE_ONCE(g_ac_last_uid, cuid);
	WRITE_ONCE(g_ac_last_bytes, bytes);
	WRITE_ONCE(g_ac_last_key, k);
	WRITE_ONCE(g_ac_last_keyoff, q);
	if (cuid && ac_uid_listed(cuid))
		atomic64_inc(&g_ac_uid_hit);
	{
		u32 hl = bytes - q;

		if (hl > AC_HDUMP_BYTES)
			hl = AC_HDUMP_BYTES;
		memcpy(g_ac_hdump, ac_buf + q, hl);
		WRITE_ONCE(g_ac_hdump_len, hl);
		WRITE_ONCE(g_ac_hdump_off, q);
	}
}

/* 分页只读拷贝：整块 copy_from_user_nofault 只要有一页未驻留就整体失败，
 * 实测大量 parcel 因此被丢弃（ac_bad 远大于 ac_scan）。改成一页一页抄，
 * 失败的页清零跳过，既不会触发缺页也照样读到已驻留的部分。全程只读。 */
static u32 __nocfi noinline ac_copy_in(const void __user *from, u32 bytes)
{
	u32 off = 0, good = 0;
	const char __user *p = (const char __user *)from;

	while (off < bytes) {
		u32 chunk = bytes - off;
		if (chunk > 4096)
			chunk = 4096;
		if (copy_from_user_nofault(ac_buf + off, p + off, chunk))
			memset(ac_buf + off, 0, chunk);
		else
			good += chunk;
		off += chunk;
	}
	return good;
}

static int __nocfi ac_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
	void *alloc = (void *)regs->regs[0];
	const void __user *from = (const void __user *)regs->regs[3];
	unsigned long bytes = regs->regs[4];
	u32 k, klen, pos, q, i;
	int pid = -1;

	atomic64_inc(&g_ac_calls);
	if (!from || bytes < 32)
		return 0;
	if (!READ_ONCE(g_ac_enable))
		return 0;
	if (bytes > AC_SCAN_BYTES) {
		atomic64_inc(&g_ac_big);
		bytes = AC_SCAN_BYTES;
	}
	if (!spin_trylock(&ac_lock))
		return 0;
	if (!ac_copy_in(from, bytes)) {
		atomic64_inc(&g_ac_bad);
		spin_unlock(&ac_lock);
		return 0;
	}
	atomic64_inc(&g_ac_scan);
	/* 反向方向优先：若这份 parcel 的落点（alloc->pid）是白名单进程，
	 * 说明数据正被装进目标 app 的 binder buffer —— aid 就在这块数据里
	 * （由 system_server 的字符串 reply 携带）。直接整块扫 16 位 hex。 */
	{
		int dstpid = -1;
		u32 duid = 0;
		if (alloc && g_ac_pid_off != (size_t)-1)
			memcpy(&dstpid, (char *)alloc + g_ac_pid_off,
			       sizeof(dstpid));
		if (dstpid > 0)
			duid = ac_pid_to_uid(dstpid);
		if (duid && ac_uid_listed(duid)) {
			u32 q2, hitq = 0;
			bool found = false;
			atomic64_inc(&g_ac_dst_seen);
			atomic64_inc(&g_ac_dst_scan);
			for (q2 = 0; q2 + 4 + AID_LEN * 2 <= bytes; q2++) {
				u32 len;
				memcpy(&len, ac_buf + q2, 4);
				if (len != AID_LEN)
					continue;
				for (i = 0; i < AID_LEN; i++) {
					u8 lo = ac_buf[q2 + 4 + i * 2];
					u8 hi = ac_buf[q2 + 4 + i * 2 + 1];
					if (hi != 0 || !aid_is_hex(lo))
						break;
				}
				if (i == AID_LEN) {
					hitq = q2;
					found = true;
					break;
				}
				for (i = 0; i < AID_LEN; i++)
					if (!aid_is_hex(ac_buf[q2 + 4 + i]))
						break;
				if (i == AID_LEN) {
					hitq = q2;
					found = true;
					break;
				}
			}
			if (found) {
				ac_rec_hit(alloc, (u32)bytes, hitq, -2);
				WRITE_ONCE(g_ac_last_dstpid, dstpid);
				WRITE_ONCE(g_ac_last_dstuid, duid);
				atomic64_inc(&g_ac_dst_hit);
				spin_unlock(&ac_lock);
				return 0;
			}
		}
	}
	/* 白名单 uid 的 parcel：不看 key，直接在整块数据里找 16 位 hex 值。
	 * Bundle 里的 String 布局是 int32 长度=16 紧跟 16 个字符，
	 * UTF-16LE 与 ASCII 两种编码都试，命中即取证返回。 */
	{
		u32 cuid = __kuid_val(current_uid());

		if (cuid && ac_uid_listed(cuid)) {
			u32 q2, hitq = 0;
			bool found = false;

			for (q2 = 0; q2 + 4 + AID_LEN * 2 <= bytes; q2++) {
				u32 len;

				memcpy(&len, ac_buf + q2, 4);
				if (len != AID_LEN)
					continue;
				for (i = 0; i < AID_LEN; i++) {
					u8 lo = ac_buf[q2 + 4 + i * 2];
					u8 hi = ac_buf[q2 + 4 + i * 2 + 1];

					if (hi != 0 || !aid_is_hex(lo))
						break;
				}
				if (i == AID_LEN) {
					hitq = q2;
					found = true;
					break;
				}
				for (i = 0; i < AID_LEN; i++)
					if (!aid_is_hex(ac_buf[q2 + 4 + i]))
						break;
				if (i == AID_LEN) {
					hitq = q2;
					found = true;
					break;
				}
			}
			if (found) {
				ac_rec_hit(alloc, (u32)bytes, hitq, -1);
				spin_unlock(&ac_lock);
				return 0;
			}
		}
	}
	for (k = 0; k < ARRAY_SIZE(aid_keys); k++) {
		klen = aid_keys[k].chars * 2;
		for (pos = 0; pos + klen <= bytes; pos++) {
			if (memcmp(ac_buf + pos, aid_keys[k].p, klen))
				continue;
			atomic64_inc(&g_ac_key);
			/* key 命中就先把发送方快照下来：即使后面找不到 16 位
			 * 回值，也能从 ac_last 看出「哪个 pid、多大 parcel、
			 * key 落在第几个字节」，用来判断截断/偏移问题。 */
			if (alloc && g_ac_pid_off != (size_t)-1)
				memcpy(&pid, (char *)alloc + g_ac_pid_off,
				       sizeof(pid));
			WRITE_ONCE(g_ac_last_pid, pid);
			WRITE_ONCE(g_ac_last_bytes, (u32)bytes);
			WRITE_ONCE(g_ac_last_keyoff, pos);
			WRITE_ONCE(g_ac_last_key, (int)k);
			/* copy_from_user 发生在发送方进程上下文，current 就是发送方；
			 * alloc->pid 是 buffer 属主（接收方），两者语义不同。
			 * uid 白名单判定必须用 current。 */
			{
				u32 cuid = __kuid_val(current_uid());
				WRITE_ONCE(g_ac_last_uid, cuid);
				if (cuid && ac_uid_listed(cuid))
					atomic64_inc(&g_ac_uid_hit);
			}
			{
				u32 dl = bytes - pos;
				if (dl > AC_DUMP_BYTES)
					dl = AC_DUMP_BYTES;
				memcpy(g_ac_dump, ac_buf + pos, dl);
				WRITE_ONCE(g_ac_dump_len, dl);
			}
			for (q = pos + klen;
			     q + 4 + AID_LEN * 2 <= bytes; q++) {
				u32 len;

				memcpy(&len, ac_buf + q, 4);
				if (len != AID_LEN)
					continue;
				for (i = 0; i < AID_LEN; i++) {
					u8 lo = ac_buf[q + 4 + i * 2];
					u8 hi = ac_buf[q + 4 + i * 2 + 1];

					if (hi != 0 || !aid_is_hex(lo))
						break;
				}
				if (i != AID_LEN)
					continue;
				ac_rec_hit(alloc, (u32)bytes, q, (int)k);
				spin_unlock(&ac_lock);
				return 0;
			}
		}
	}
	spin_unlock(&ac_lock);
	return 0;
}

struct kb_req {
	void *dest;
	u32 bytes;
};
static struct kretprobe g_kb_rp;
static atomic64_t g_kb_calls, g_kb_scan, g_kb_hit;
static atomic64_t g_kb_off;
static atomic64_t g_kb_big, g_kb_nouid;
/* v7.2-A 诊断模式：ret 阶段完全不读 dest，只统计 parcel 长度分布，
 * 用来判定死机到底来自 "读 dest 内存" 还是 "hook 本身"。 */
static atomic64_t g_kb_sz[4];	/* <=64 / <=256 / <=1K / <=4K */
static atomic64_t g_kb_bytesum;
static atomic64_t g_kb_bad;	/* dest 不在内核高半区 / 越界读数 */
static atomic64_t g_kb_rd;	/* B 阶段已真正读过的 parcel 数 */
#define KB_VERBOSE 2		/* 0=只计数 1=v7.1 全量扫描 2=只读前 64B 扫描 */
#define KB_READ_BYTES 64	/* v7.3-B：最多读这么多字节，命中 4+16*2 足够 */
#define KB_READ_CAP  2000	/* B 阶段一次 arm 最多真正读这么多 parcel */
static bool g_kb_stop;	/* 命中或超上限后置位：原子上下文里不能 unregister，
			 * 只把后续扫描全部短路，proc 里能看到 stop=1 */
/* v7.1 硬上限：kretprobe 跑在 atomic 上下文，全系统 binder reply 高频，
 * 无上限的逐字节长扫描会把看门狗拖死（v7 死机复盘）。 */
#define KB_MAX_BYTES  4096	/* 单个 parcel 超过这个长度一律跳过，不截断 */
#define KB_SCAN_CAP   20000	/* 一次 arm 期间最多扫这么多 parcel */
/* 在 kernel buffer 里找 [int32 len=16][16 位 hex]，UTF-16LE 与 ASCII 都试。
 * 先用 memchr 找 0x10 字节做 O(1) 预过滤，避免对 4KB 逐字节跑循环。 */
static bool __maybe_unused __nocfi noinline kb_find_aid(const u8 *b, u32 bytes, u32 *out)
{
	u32 q = 0, i;
	u32 last;
	if (bytes < 4 + AID_LEN * 2)
		return false;
	last = bytes - 4 - AID_LEN * 2;	/* q 的合法上界 */
	while (q <= last) {
		const u8 *p = memchr(b + q, (int)(u8)AID_LEN, (size_t)(last - q + 1));
		if (!p)
			break;
		q = (u32)(p - b);
		if (p[1] || p[2] || p[3]) {
			q++;
			continue;
		}
		for (i = 0; i < AID_LEN; i++) {
			u8 lo = b[q + 4 + i * 2];
			u8 hi = b[q + 4 + i * 2 + 1];
			if (hi != 0 || !aid_is_hex(lo))
				break;
		}
		if (i == AID_LEN) {
			*out = q;
			return true;
		}
		for (i = 0; i < AID_LEN; i++)
			if (!aid_is_hex(b[q + 4 + i]))
				break;
		if (i == AID_LEN) {
			*out = q;
			return true;
		}
		q++;
	}
	return false;
}
static int __nocfi kb_entry_handler(struct kretprobe_instance *ri,
				    struct pt_regs *regs)
{
	struct kb_req *rq = (struct kb_req *)ri->data;
	void *dest = (void *)regs->regs[1];
	unsigned long bytes = regs->regs[3];
	u32 cuid;
	if (!dest || bytes < 32)
		return 0;
	if (READ_ONCE(g_kb_stop))
		return 0;
	cuid = __kuid_val(current_uid());
	/* 4294967295 = 不限制 uid，只在 dry-run 之外有意义；kb 探针在
	 * atomic 上下文里跑，绝不允许全域扫描（v7 死机的直接触发条件）。 */
	if (!cuid || cuid == (u32)-1 || !ac_uid_listed(cuid)) {
		atomic64_inc(&g_kb_nouid);
		return 0;
	}
	if (bytes > KB_MAX_BYTES) {
		atomic64_inc(&g_kb_big);
		return 0;
	}
	if (atomic64_inc_return(&g_kb_calls) > KB_SCAN_CAP) {
		WRITE_ONCE(g_kb_stop, true);
		return 0;
	}
	rq->dest = dest;
	rq->bytes = (u32)bytes;
	return 1;
}
static int __nocfi kb_ret_handler(struct kretprobe_instance *ri,
				  struct pt_regs *regs)
{
	struct kb_req *rq = (struct kb_req *)ri->data;
	u32 bytes = rq->bytes, off = 0, n;
	u8 tmp[KB_READ_BYTES];
	u64 da;
	int bk;
	(void)regs;
	if (!rq->dest || !bytes)
		return 0;
	if (READ_ONCE(g_kb_stop))
		return 0;
	atomic64_inc(&g_kb_scan);
	atomic64_add(bytes, &g_kb_bytesum);
#if KB_VERBOSE == 0
	/* v7.2-A：不碰 dest 内存，只做长度分桶，判定死机来源 */
	bk = bytes <= 64 ? 0 : bytes <= 256 ? 1 : bytes <= 1024 ? 2 : 3;
	atomic64_inc(&g_kb_sz[bk]);
	(void)off;
	(void)n;
#elif KB_VERBOSE == 2
	/* v7.3-B：只读前 64 字节，且先校验地址在内核高半区 */
	bk = bytes <= 64 ? 0 : bytes <= 256 ? 1 : bytes <= 1024 ? 2 : 3;
	atomic64_inc(&g_kb_sz[bk]);
	if (bytes < 4 + AID_LEN * 2)
		return 0;
	da = (u64)(unsigned long)rq->dest;
	if (da < 0xffff000000000000ULL) {
		atomic64_inc(&g_kb_bad);
		return 0;
	}
	if (atomic64_inc_return(&g_kb_rd) > KB_READ_CAP)
		return 0;
	n = bytes < KB_READ_BYTES ? bytes : KB_READ_BYTES;
	memcpy(tmp, rq->dest, n);
	if (!kb_find_aid(tmp, n, &off))
		return 0;
	atomic64_inc(&g_kb_hit);
	WRITE_ONCE(g_kb_stop, true);	/* 抓到就停：不再反复读 */
	atomic64_set(&g_kb_off, off);
	memcpy(g_ac_dump, tmp, n);
	WRITE_ONCE(g_ac_dump_len, n);
#else
	if (!kb_find_aid((const u8 *)rq->dest, bytes, &off))
		return 0;
	atomic64_inc(&g_kb_hit);
	WRITE_ONCE(g_kb_stop, true);	/* 抓到就停：不再扫，避免反复 memcpy */
	atomic64_set(&g_kb_off, off);
	n = bytes;
	if (n > AC_DUMP_BYTES)
		n = AC_DUMP_BYTES;
	memcpy(g_ac_dump, rq->dest, n);
	WRITE_ONCE(g_ac_dump_len, n);
#endif
	return 0;
}
static int ac_arm_locked(void)
{
	int ret;

	if (g_ac_armed)
		return 0;
	g_ac_kp.symbol_name = "binder_alloc_copy_user_to_buffer";
	g_ac_kp.addr = NULL;
	g_ac_kp.pre_handler = ac_pre_handler;
	ret = register_kprobe(&g_ac_kp);
	if (ret) {
		g_ac_kp.addr = NULL;
		pr_warn("aidcopy probe failed on %s: %d\n",
			g_ac_kp.symbol_name, ret);
		return ret;
	}
	g_ac_armed = true;
	pr_info("aidcopy probe armed on %s\n", g_ac_kp.symbol_name);
	/* v7：reply 方向改读内核侧 binder buffer —— binder_alloc_copy_from_buffer
	 * (alloc, dest, buffer_off, bytes) 的 dest 是内核缓冲，返回点直接扫，
	 * 不受 mm 属主与缺页影响；归属用 current（这时正是收 reply 的进程）。 */
	memset(&g_kb_rp, 0, sizeof(g_kb_rp));
	WRITE_ONCE(g_kb_stop, false);
	atomic64_set(&g_kb_calls, 0);
	atomic64_set(&g_kb_scan, 0);
	atomic64_set(&g_kb_hit, 0);
	atomic64_set(&g_kb_off, 0);
	atomic64_set(&g_kb_big, 0);
	atomic64_set(&g_kb_nouid, 0);
	memset(g_kb_sz, 0, sizeof(g_kb_sz));
	atomic64_set(&g_kb_bytesum, 0);
	atomic64_set(&g_kb_bad, 0);
	atomic64_set(&g_kb_rd, 0);
	g_kb_rp.kp.symbol_name = "binder_alloc_copy_from_buffer";
	g_kb_rp.entry_handler = kb_entry_handler;
	g_kb_rp.handler = kb_ret_handler;
	g_kb_rp.data_size = sizeof(struct kb_req);
	g_kb_rp.maxactive = 32;
	ret = register_kretprobe(&g_kb_rp);
	if (ret) {
		g_kb_rp.kp.addr = NULL;
		atomic64_inc(&g_ac_nosym);
		pr_warn("aidcopy: kb probe failed on %s: %d\n",
			g_kb_rp.kp.symbol_name, ret);
	} else {
		pr_info("aidcopy: kb probe armed on %s\n",
			g_kb_rp.kp.symbol_name);
	}
	return 0;
}

static void ac_disarm_locked(void)
{
	if (!g_ac_armed)
		return;
	unregister_kprobe(&g_ac_kp);
	g_ac_kp.addr = NULL;
	if (g_kb_rp.kp.addr || g_kb_rp.kp.symbol_name) {
		unregister_kretprobe(&g_kb_rp);
		g_kb_rp.kp.addr = NULL;
		g_kb_rp.kp.symbol_name = NULL;
	}
	g_ac_armed = false;
	pr_info("aidcopy probe disarmed\n");
}

static void ac_init(void)
{
	int rc;

	g_ac_kp.pre_handler = ac_pre_handler;
	rc = susfs_btf_member_offset("binder_alloc", "pid", "int",
				     &g_ac_pid_off);
	if (rc) {
		g_ac_pid_off = (size_t)-1;
		pr_warn("aidcopy: binder_alloc.pid unresolved (%d)\n", rc);
	}
	rc = susfs_btf_member_offset("cred", "uid", "kuid_t",
				     &g_ac_cred_uid_off);
	g_ac_cred_uid_ok = !rc;
	if (rc) {
		/* struct cred { atomic_long_t usage; kuid_t uid; ... } */
		g_ac_cred_uid_off = 8;
		pr_warn("aidcopy: cred.uid unresolved (%d), assume 8\n", rc);
	}
	ac_find_task_fn = (void *)ksu_kallsyms_lookup_name("find_get_task_by_vpid");
	ac_task_cred_fn = (void *)ksu_kallsyms_lookup_name("get_task_cred");
	if (!ac_find_task_fn || !ac_task_cred_fn) {
		atomic64_inc(&g_ac_nosym);
		pr_warn("aidcopy: uid resolver missing\n");
	}
}

static bool drm_patch_write_parcel_loose(struct binder_transaction_data *td);
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
		aid_try_patch(&td);
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
cap = bwr->write_size;
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
/* 不再按事务码过滤：DRM 的 code 不是 11，原先的门槛让 HAL 侧回复永远被
     * w_skipped 挡掉。现在对白名单 uid 的任何请求都开一个 60s 窗口，
     * 回复侧仍要求 [status=0][len=32] + 32 字节非零才写。 */
    (void)td.code;
    {
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
if (!drm_patch_write_parcel(&td))
drm_patch_write_parcel_loose(&td);
}
off += step;
}
}

static int drm_entry_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
struct drm_call_ctx *ctx = (struct drm_call_ctx *)ri->data;

	if (!READ_ONCE(g_enable) && !READ_ONCE(g_aid_enable))
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
/* [v16.60][P0] 在 entry 侧就开窗口：HAL 的 BC_REPLY 往往在 DevCheck 的
 * ret_handler 之前发出，若只在 ret 侧设 g_pending，HAL 侧永远错过窗口。 */
if (ctx->cmd == BINDER_WRITE_READ && ctx->arg) {
struct binder_write_read wb;
if (!drm_u_read(&wb, (void __user *)drm_untag((unsigned long)ctx->arg), sizeof(wb)) &&
    wb.write_size && wb.write_buffer)
drm_scan_request(&wb);
}
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
.maxactive = 1024,
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
/* [v16.67][P0] nmissed：kretprobe 实例池耗尽时，内核在 pre_handler 里拿不到
 * 空闲实例，只会把 nmissed 加一、不调用 entry_handler。用户态 watchdog 据此
 * 精确判定「池耗尽」而不是「只是没流量」。
 */
seq_printf(m, "nmissed=%u\n", g_krp.nmissed);
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
	seq_printf(m, "aid_enable=%d aid_apply=%d aid=%s\n",
		READ_ONCE(g_aid_enable), READ_ONCE(g_aid_apply),
		g_aid[0] ? g_aid : "-");
	seq_printf(m, "aid_scan=%lld aid_key_hits=%lld aid_hits=%lld aid_written=%lld\n",
		atomic64_read(&g_aid_scan), atomic64_read(&g_aid_key_hits),
		atomic64_read(&g_aid_hits), atomic64_read(&g_aid_written));
	seq_printf(m, "aidcopy_enable=%d ac_armed=%d\n",
		READ_ONCE(g_ac_enable), READ_ONCE(g_ac_armed));
	seq_printf(m, "ac_calls=%lld ac_scan=%lld ac_key=%lld ac_hit=%lld ac_uid_hit=%lld\n",
		atomic64_read(&g_ac_calls), atomic64_read(&g_ac_scan),
		atomic64_read(&g_ac_key), atomic64_read(&g_ac_hit),
		atomic64_read(&g_ac_uid_hit));
	seq_printf(m, "ac_big=%lld ac_bad=%lld ac_nosym=%lld\n",
		atomic64_read(&g_ac_big), atomic64_read(&g_ac_bad),
		atomic64_read(&g_ac_nosym));
	seq_printf(m, "ac_last pid=%d uid=%d bytes=%d key=%d koff=%d\n",
		READ_ONCE(g_ac_last_pid), READ_ONCE(g_ac_last_uid),
		(int)READ_ONCE(g_ac_last_bytes), READ_ONCE(g_ac_last_key),
		(int)READ_ONCE(g_ac_last_keyoff));
	seq_printf(m, "ac_dst scan=%lld hit=%lld lastpid=%d lastuid=%d\n",
		atomic64_read(&g_ac_dst_scan), atomic64_read(&g_ac_dst_hit),
		READ_ONCE(g_ac_last_dstpid), READ_ONCE(g_ac_last_dstuid));
	seq_printf(m, "ac_dst_seen=%lld\n", atomic64_read(&g_ac_dst_seen));
	seq_printf(m, "kb calls=%lld scan=%lld hit=%lld off=%lld\n",
		atomic64_read(&g_kb_calls), atomic64_read(&g_kb_scan),
		atomic64_read(&g_kb_hit), atomic64_read(&g_kb_off));
	seq_printf(m, "kb stop=%d big=%lld nouid=%lld cap=%d max=%d\n",
		READ_ONCE(g_kb_stop) ? 1 : 0,
		atomic64_read(&g_kb_big), atomic64_read(&g_kb_nouid),
		KB_SCAN_CAP, KB_MAX_BYTES);
	seq_printf(m, "kb rd=%lld bad=%lld\n",
		atomic64_read(&g_kb_rd), atomic64_read(&g_kb_bad));
	seq_printf(m, "kb sz 64=%lld 256=%lld 1k=%lld 4k=%lld sum=%lld v=%d\n",
		atomic64_read(&g_kb_sz[0]), atomic64_read(&g_kb_sz[1]),
		atomic64_read(&g_kb_sz[2]), atomic64_read(&g_kb_sz[3]),
		atomic64_read(&g_kb_bytesum), KB_VERBOSE);
	{
		u32 i, n = READ_ONCE(g_ac_dump_len);
		if (n) {
			if (n > AC_DUMP_BYTES)
				n = AC_DUMP_BYTES;
			seq_puts(m, "ac_dump ");
			for (i = 0; i < n; i++)
				seq_printf(m, "%02x", g_ac_dump[i]);
			seq_puts(m, "\n");
		}
	}
	{
		u32 i, n = READ_ONCE(g_ac_hdump_len);
		if (n) {
			if (n > AC_HDUMP_BYTES)
				n = AC_HDUMP_BYTES;
			seq_printf(m, "ac_hdump off=%d ",
				   (int)READ_ONCE(g_ac_hdump_off));
			for (i = 0; i < n; i++)
				seq_printf(m, "%02x", g_ac_hdump[i]);
			seq_puts(m, "\n");
		}
	}
	return 0;
}

static int drm_proc_open(struct inode *inode, struct file *file)
{
/* 与其它控制节点同一道门。之前这里没有检查：节点 0644 且不在
 * susfs_self_hide_paths，非 root 直接 cat 就能读到 mode/enabled/armed/
 * probe/uids/haluid —— 比 tainted 更直接地暴露模块存在。
 * 答 ENOENT 而不是 EACCES：EACCES 等于承认节点存在。 */
if (!susfs_control_visible())
return -ENOENT;
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

/* 理由同 open：节点进了 self_hide_paths 之后 sus_path_relax_mode() 会把它
 * 放宽成 0777，一个在降权之前就拿到的 fd 不能成为写入通道。 */
if (!susfs_control_visible())
return -ENOENT;
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
/* [v16.82][P0] aid 与 DRM 共用同一个 kretprobe 实例：只关 DRM 时不能顺手
 * 把探针也卸掉，否则面板点「DRM 还原」会连带让 Android ID 伪装失效。 */
if (!READ_ONCE(g_aid_enable))
drm_disarm_locked();
mutex_unlock(&g_ctl_lock);
	} else if (!strncmp(buf, "aid ", 4)) {
		const char *s = buf + 4;
		int i;
		if (strlen(s) != AID_LEN)
			return -EINVAL;
		for (i = 0; i < AID_LEN; i++) {
			if (!aid_is_hex((u8)s[i]))
				return -EINVAL;
		}
		mutex_lock(&g_ctl_lock);
		memcpy(g_aid, s, AID_LEN);
		g_aid[AID_LEN] = 0;
		mutex_unlock(&g_ctl_lock);
		pr_info("android_id fake set\n");
	} else if (!strcmp(buf, "aidapply 1")) {
		WRITE_ONCE(g_aid_apply, true);
		pr_warn("android_id apply ON: replies will be rewritten\n");
	} else if (!strcmp(buf, "aidapply 0")) {
		WRITE_ONCE(g_aid_apply, false);
		pr_info("android_id dry-run mode\n");
	} else if (!strcmp(buf, "aidenable 1")) {
		mutex_lock(&g_ctl_lock);
		WRITE_ONCE(g_aid_enable, true);
		if (drm_arm_locked())
			WRITE_ONCE(g_aid_enable, false);
		mutex_unlock(&g_ctl_lock);
	} else if (!strcmp(buf, "aidenable 0")) {
		mutex_lock(&g_ctl_lock);
		WRITE_ONCE(g_aid_enable, false);
		if (!READ_ONCE(g_enable))
			drm_disarm_locked();
		mutex_unlock(&g_ctl_lock);
	} else if (!strcmp(buf, "aidcopy 1")) {
		mutex_lock(&g_ctl_lock);
		WRITE_ONCE(g_ac_enable, true);
		ac_arm_locked();
		if (!READ_ONCE(g_ac_armed))
			WRITE_ONCE(g_ac_enable, false);
		mutex_unlock(&g_ctl_lock);
	} else if (!strcmp(buf, "aidcopy 0")) {
		mutex_lock(&g_ctl_lock);
		WRITE_ONCE(g_ac_enable, false);
		ac_disarm_locked();
		mutex_unlock(&g_ctl_lock);
	} else if (!strcmp(buf, "reset")) {
atomic64_set(&g_stats.calls, 0);
atomic64_set(&g_stats.read_streams, 0);
atomic64_set(&g_stats.reply_seen, 0);
atomic64_set(&g_stats.exact_hits, 0);
atomic64_set(&g_stats.would_write, 0);
atomic64_set(&g_stats.written, 0);
atomic64_set(&g_stats.nofault_fail, 0);
atomic64_set(&g_aid_scan, 0);
atomic64_set(&g_aid_key_hits, 0);
atomic64_set(&g_aid_hits, 0);
atomic64_set(&g_aid_written, 0);
atomic64_set(&g_ac_calls, 0);
atomic64_set(&g_ac_scan, 0);
atomic64_set(&g_ac_key, 0);
atomic64_set(&g_ac_hit, 0);
atomic64_set(&g_ac_uid_hit, 0);
atomic64_set(&g_ac_big, 0);
atomic64_set(&g_ac_bad, 0);
atomic64_set(&g_ac_nosym, 0);
atomic64_set(&g_ac_dst_scan, 0);
atomic64_set(&g_ac_dst_hit, 0);
atomic64_set(&g_ac_dst_seen, 0);
atomic64_set(&g_kb_calls, 0);
atomic64_set(&g_kb_scan, 0);
atomic64_set(&g_kb_hit, 0);
atomic64_set(&g_kb_off, 0);
atomic64_set(&g_kb_big, 0);
atomic64_set(&g_kb_nouid, 0);
memset(g_kb_sz, 0, sizeof(g_kb_sz));
atomic64_set(&g_kb_bytesum, 0);
atomic64_set(&g_kb_bad, 0);
atomic64_set(&g_kb_rd, 0);
WRITE_ONCE(g_kb_stop, false);
WRITE_ONCE(g_ac_last_dstpid, -1);
WRITE_ONCE(g_ac_last_dstuid, 0);
WRITE_ONCE(g_ac_dump_len, 0);
WRITE_ONCE(g_ac_hdump_len, 0);
WRITE_ONCE(g_ac_last_pid, -1);
WRITE_ONCE(g_ac_last_uid, 0);
WRITE_ONCE(g_ac_last_bytes, 0);
WRITE_ONCE(g_ac_last_key, -1);
WRITE_ONCE(g_ac_last_keyoff, 0);
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
	{
		u8 rnd[8];
		int i;
		get_random_bytes(rnd, sizeof(rnd));
		for (i = 0; i < 8; i++)
			snprintf(g_aid + i * 2, 3, "%02x", rnd[i]);
		g_aid[AID_LEN] = 0;
	}
	g_aid_enable = false;
	g_aid_apply = false;
	g_ac_enable = false;
	g_ac_armed = false;
	ac_init();
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
ac_disarm_locked();
mutex_unlock(&g_ctl_lock);
if (g_proc) {
proc_remove(g_proc);
g_proc = NULL;
}
}
